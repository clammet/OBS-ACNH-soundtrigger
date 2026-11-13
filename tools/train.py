"""Compile clean, repeatable sound effects into small spectral templates."""
import argparse
import json
from pathlib import Path

import numpy as np

from audio import SAMPLE_RATE, FFT_SIZE, HOP_SIZE, decode, spectrum, features


def train(args):
    paths = sorted(args.sounds.glob("*.wav"))
    if not paths:
        raise ValueError(f"No WAV files found in {args.sounds}")
    if len(paths) > 64:
        raise ValueError("At most 64 templates are supported")
    frames = round((args.window * SAMPLE_RATE - FFT_SIZE) / HOP_SIZE) + 1
    if not 4 <= frames <= 128:
        raise ValueError("Window must produce between 4 and 128 analysis frames")
    if not 0 < args.threshold <= 1 or not 0.1 <= args.cooldown <= 60 or not -120 <= args.minimum_rms_db <= 0:
        raise ValueError("Invalid threshold, cooldown, or minimum RMS")
    if not 1 <= args.bands_per_sound <= 16 or not 200 <= args.min_frequency <= 7500:
        raise ValueError("Invalid band selection parameters")
    spectra = []
    trims = []
    for path in paths:
        audio = decode(path)
        # Trim leading silence using 4 ms RMS blocks, retaining 8 ms before onset.
        block = 64
        count = len(audio) // block
        if count == 0:
            raise ValueError(f"{path.name}: file is too short")
        rms = np.sqrt(np.mean(audio[:count * block].reshape(-1, block) ** 2, axis=1))
        if rms.max() < 1e-7:
            raise ValueError(f"{path.name}: file is silent")
        onset = max(0, int(np.flatnonzero(rms >= rms.max() * 0.05)[0]) * block - 128)
        audio = audio[onset:]
        needed = FFT_SIZE + (frames - 1) * HOP_SIZE
        audio = np.pad(audio[:needed], (0, max(0, needed - len(audio))))
        spectra.append(spectrum(audio))
        trims.append(onset / SAMPLE_RATE)

    bins = []
    for power in spectra:
        energy = power.sum(axis=0).copy()
        energy[:max(6, int(np.ceil(args.min_frequency * FFT_SIZE / SAMPLE_RATE)))] = 0
        energy[FFT_SIZE // 2 - 5:] = 0
        for _ in range(args.bands_per_sound):
            b = int(energy.argmax())
            if energy[b] < 1e-15:
                break
            if not any(abs(b - existing) <= 3 for existing in bins):
                bins.append(b)
            energy[max(0, b - 3):b + 4] = 0
    bins.sort()
    if not bins or len(bins) > 64:
        raise ValueError("Model needs between 1 and 64 bands; reduce --bands-per-sound")

    values = []
    for power in spectra:
        value = features(power, bins)
        norm = np.linalg.norm(value)
        if norm < 1e-10:
            raise ValueError("No usable tonal energy; try lowering --min-frequency")
        values.append(value / norm)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as output:
        output.write(f"SOUNDTRIGGER 1\n{SAMPLE_RATE} {FFT_SIZE} {HOP_SIZE}\n")
        output.write(f"{frames} {len(bins)} {len(paths)}\n")
        output.write(f"{args.threshold:.9g} {args.cooldown:.9g} {args.minimum_rms_db:.9g}\n")
        output.write(" ".join(map(str, bins)) + "\n")
        for path, value in zip(paths, values):
            output.write(json.dumps(path.stem, ensure_ascii=False) + "\n")
            for row in value:
                output.write(" ".join(f"{v:.9g}" for v in row) + "\n")
    summary = {
        "model": args.output.name,
        "templates": [p.name for p in paths],
        "leading_silence_trimmed_seconds": trims,
        "bands_hz": [b * SAMPLE_RATE / FFT_SIZE for b in bins],
        "window_seconds": (FFT_SIZE + (frames - 1) * HOP_SIZE) / SAMPLE_RATE,
        "frames": frames, "bands": len(bins), "threshold": args.threshold,
        "bytes": args.output.stat().st_size,
    }
    print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sounds", type=Path, help="Folder of clean WAV references")
    parser.add_argument("output", type=Path, help="Output .stmodel file")
    parser.add_argument("--window", type=float, default=0.656, help="Seconds to match from cue onset")
    parser.add_argument("--bands-per-sound", type=int, default=6)
    parser.add_argument("--min-frequency", type=float, default=1000)
    parser.add_argument("--threshold", type=float, default=0.90)
    parser.add_argument("--cooldown", type=float, default=1.0, help="Seconds between triggers")
    parser.add_argument("--minimum-rms-db", type=float, default=-75)
    args = parser.parse_args()
    try:
        train(args)
    except (ValueError, OSError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
