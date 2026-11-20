#!/usr/bin/env python3
# Crisp CI checks: the test signal for the sound null test (ci-sw/README.md). Fixed formulas and a fixed noise seed;
# ci-sw/signal/SHA256SUMS pins the files, so every platform renders exactly the same input.
# SPDX-License-Identifier: GPL-3.0-or-later. Copyright (C) 2026 Sonic Weaponry.
# Usage: python3 make_test_signal.py <out folder>   (needs numpy)
import os
import struct
import sys

import numpy as np


def write_wav(path, rate, mono):
    data = mono.astype("<f4").tobytes()
    header = b"RIFF" + struct.pack("<I", 4 + 26 + 12 + 8 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHHH", 18, 3, 1, rate, rate * 4, 4, 32, 0)
    header += b"fact" + struct.pack("<II", 4, len(mono))
    header += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(header + data)


def signal(rate):
    parts = []
    t = np.arange(int(2.0 * rate)) / rate
    f0, f1, length = 20.0, 20000.0, 2.0  # log sine sweep, -6 dBFS
    k = np.log(f1 / f0)
    parts.append(0.5 * np.sin(2 * np.pi * f0 * length / k * (np.exp(t / length * k) - 1)))
    parts.append(np.zeros(int(0.25 * rate)))
    # A bass note: 55 Hz saw (8 harmonics), stepped from -30 to 0 dBFS in 6 steps of 0.25 s
    t = np.arange(int(0.25 * rate)) / rate
    saw = sum(np.sin(2 * np.pi * 55.0 * n * t) / n for n in range(1, 9)) * (2 / np.pi)
    for db in (-30, -24, -18, -12, -6, 0):
        parts.append(saw / np.max(np.abs(saw)) * 10 ** (db / 20))
    parts.append(np.zeros(int(0.25 * rate)))
    rng = np.random.default_rng(20261119)  # white noise burst, -12 dBFS peak
    noise = rng.uniform(-1, 1, int(0.5 * rate))
    parts.append(noise * 10 ** (-12 / 20))
    parts.append(np.zeros(int(0.25 * rate)))
    clicks = np.zeros(int(0.5 * rate))  # impulses
    clicks[:: int(0.1 * rate)] = 0.9
    parts.append(clicks)
    parts.append(np.zeros(int(0.25 * rate)))
    return np.concatenate(parts).astype(np.float32)


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for rate in (44100, 48000):
        write_wav(os.path.join(out, "signal-%d.wav" % rate), rate, signal(rate))
