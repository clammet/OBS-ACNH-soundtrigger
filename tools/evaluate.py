"""Replay a recording through the exact C++ detector used by the OBS plugin."""
import argparse
import csv
import io
import json
from pathlib import Path
import shlex
import subprocess

import numpy as np
from scipy import signal

from audio import SAMPLE_RATE, FFT_SIZE, HOP_SIZE, decode, spectrum, features


def load_model(path):
    tokens = iter(shlex.split(path.read_text()))
    assert next(tokens) == "SOUNDTRIGGER" and int(next(tokens)) == 1
    assert [int(next(tokens)) for _ in range(3)] == [SAMPLE_RATE, FFT_SIZE, HOP_SIZE]
    frames, bands, count = [int(next(tokens)) for _ in range(3)]
    threshold, cooldown, minimum_rms_db = [float(next(tokens)) for _ in range(3)]
    bins = [int(next(tokens)) for _ in range(bands)]
    templates = []
    names = []
    for _ in range(count):
        names.append(next(tokens))
        templates.append(np.array([float(next(tokens)) for _ in range(frames * bands)]).reshape(frames, bands))
    return frames, bins, names, templates, threshold, cooldown, minimum_rms_db


def timestamps(path):
    output = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = list(map(float, line.split(":")))
        if len(parts) not in (2, 3) or any(v < 0 for v in parts) or any(v >= 60 for v in parts[1:]):
            raise ValueError(f"Invalid timecode: {line}")
        seconds = 0
        for part in parts:
            seconds = seconds * 60 + part
        output.append(seconds)
    return output


def replay(executable, model, audio, scores=None):
    decoder = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", str(audio), "-vn", "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "f32le", "-"],
        stdout=subprocess.PIPE,
    )
    try:
        command = [str(executable.resolve()), str(model)]
        if scores:
            command.append(str(scores))
        result = subprocess.run(command, stdin=decoder.stdout, capture_output=True, text=True)
    finally:
        decoder.stdout.close()
    decoder_result = decoder.wait()
    if result.returncode:
        raise RuntimeError(result.stderr)
    if decoder_result:
        raise RuntimeError("ffmpeg could not decode the feed")
    return list(csv.DictReader(io.StringIO(result.stdout))), json.loads(result.stderr)


def reference_error(audio_path, model, native_scores):
    frames, bins, _, templates, _, _, minimum_rms_db = load_model(model)
    audio = decode(audio_path)
    feat = features(spectrum(audio), bins).astype(np.float64)
    energy = signal.convolve(np.sum(feat ** 2, axis=1), np.ones(frames), mode="valid", method="direct")
    sample_frames = np.lib.stride_tricks.sliding_window_view(audio, FFT_SIZE)[::HOP_SIZE]
    rms_energy = np.empty(len(sample_frames))
    for i in range(0, len(sample_frames), 4096):
        rms_energy[i:i + 4096] = np.mean(sample_frames[i:i + 4096] ** 2, axis=1)
    rms = signal.convolve(rms_energy, np.ones(frames) / frames, mode="valid", method="direct")
    best = np.zeros(len(energy))
    for template in templates:
        dots = sum(signal.correlate(feat[:, b], template[:, b], mode="valid", method="fft") for b in range(len(bins)))
        score = dots / np.sqrt(np.maximum(energy, 1e-20))
        score[(energy < 1e-20) | (rms < 10 ** (minimum_rms_db / 10))] = 0
        best = np.maximum(best, np.minimum(score, 1))
    best = np.pad(best, (frames - 1, 0))
    error = float(np.max(np.abs(best - native_scores["score"])))
    if error > 0.0001:
        raise RuntimeError(f"Python/native score mismatch: {error}")
    return error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("audio", type=Path)
    parser.add_argument("--known", type=Path, required=True)
    parser.add_argument("--negative", type=Path, required=True)
    parser.add_argument("--replay", type=Path, default=Path("build/soundtrigger-replay"))
    parser.add_argument("--output", type=Path, default=Path("build/evaluation"))
    parser.add_argument("--tolerance", type=float, default=1.5, help="Approximate onset timecode tolerance in seconds")
    parser.add_argument("--verify-reference", action="store_true", help="Compare every native frame against NumPy/SciPy")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frames, bins, names, _, threshold, cooldown, _ = load_model(args.model)
    window = (FFT_SIZE + (frames - 1) * HOP_SIZE) / SAMPLE_RATE
    score_path = args.output / "scores.csv"
    raw_events, _ = replay(args.replay, args.model, args.audio, score_path)
    # A separate pass excludes score CSV writing from the benchmark.
    _, benchmark = replay(args.replay, args.model, args.audio)
    data = np.genfromtxt(score_path, delimiter=",", names=True)
    onsets = data["end_seconds"] - window
    events = [dict(end_seconds=float(e["end_seconds"]), onset_seconds=float(e["end_seconds"]) - window,
                   score=float(e["score"]), template=names[int(e["template_index"])]) for e in raw_events]
    matches = []
    used = set()
    for known in timestamps(args.known):
        options = [(abs(e["onset_seconds"] - known), i) for i, e in enumerate(events)
                   if i not in used and abs(e["onset_seconds"] - known) <= args.tolerance]
        index = min(options)[1] if options else None
        if index is not None:
            used.add(index)
        mask = np.abs(onsets - known) <= args.tolerance
        matches.append(dict(known_seconds=known, event=events[index] if index is not None else None,
                            peak_score=float(data["score"][mask].max()) if mask.any() else None))
    negatives = []
    for negative in timestamps(args.negative):
        mask = np.abs(onsets - negative) <= args.tolerance
        negatives.append(dict(known_seconds=negative, peak_score=float(data["score"][mask].max()),
                              triggers=sum(abs(e["onset_seconds"] - negative) <= args.tolerance for e in events)))
    report = dict(
        audio=args.audio.name, model=args.model.name, threshold=threshold, cooldown_seconds=cooldown,
        analysis_window_seconds=window, frequency_bands=len(bins), templates=len(names),
        timecode_tolerance_seconds=args.tolerance,
        labeled_hits=len(matches), detected_labeled_hits=sum(m["event"] is not None for m in matches),
        suspected_false_positive_checks=negatives, labeled_hit_details=matches,
        unlabeled_detections=[e for i, e in enumerate(events) if i not in used], benchmark=benchmark,
        caveat="Unlabeled detections need human review. Sparse labels cannot establish precision or a false-positive rate.",
    )
    if args.verify_reference:
        report["maximum_python_native_score_error"] = reference_error(args.audio, args.model, data)
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    with (args.output / "detections.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=["onset_seconds", "end_seconds", "score", "template"])
        writer.writeheader()
        writer.writerows(events)
    print(json.dumps(report, indent=2))
    if any(m["event"] is None for m in matches) or any(n["triggers"] for n in negatives):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
