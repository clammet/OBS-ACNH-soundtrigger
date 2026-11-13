"""Offline audio and feature extraction. No part of this runs inside OBS."""
from pathlib import Path
import subprocess

import numpy as np
from scipy import fft

SAMPLE_RATE = 16000
FFT_SIZE = 512
HOP_SIZE = 256


def decode(path: Path) -> np.ndarray:
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-vn", "-ac", "1", "-ar",
         str(SAMPLE_RATE), "-f", "f32le", "-"],
        check=True, stdout=subprocess.PIPE,
    )
    return np.frombuffer(result.stdout, dtype="<f4")


def spectrum(audio: np.ndarray) -> np.ndarray:
    if len(audio) < FFT_SIZE:
        raise ValueError("Audio must contain at least 32 ms of samples")
    frames = np.lib.stride_tricks.sliding_window_view(audio, FFT_SIZE)[::HOP_SIZE]
    window = np.hanning(FFT_SIZE).astype(np.float32)
    output = np.empty((len(frames), FFT_SIZE // 2 + 1), dtype=np.float32)
    for start in range(0, len(frames), 4096):
        output[start:start + 4096] = np.abs(fft.rfft(frames[start:start + 4096] * window)) ** 2
    return output


def features(power: np.ndarray, bins: list[int]) -> np.ndarray:
    output = np.empty((len(power), len(bins)), dtype=np.float32)
    for i, b in enumerate(bins):
        signal = power[:, b - 1:b + 2].sum(axis=1)
        noise = (power[:, b - 6:b - 3].sum(axis=1) + power[:, b + 4:b + 7].sum(axis=1)) * 0.5
        output[:, i] = np.sqrt(np.maximum(0, signal - noise))
    return output
