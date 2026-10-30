#!/usr/bin/env python3
"""Generate the noxis meow sounds with nothing but the Python stdlib.

The results are committed to the repository, so building the ISO never
needs an audio toolchain:

    configs/noxis/airootfs/usr/share/sounds/noxis/meow.wav        (login)
    configs/noxis/airootfs/usr/share/sounds/noxis/meow-off.wav    (poweroff/reboot)

Usage:  python3 scripts/make-sounds.py
"""

from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

RATE = 22050
ROOT = Path(__file__).resolve().parent.parent


def synth(dur: float, f_start: float, f_peak: float, f_end: float, gain: float) -> list[float]:
    """One cartoon meow: a small sine stack swept along the miaou contour."""
    def freq_at(p: float) -> float:
        if p < 0.35:
            return f_start + (f_peak - f_start) * (p / 0.35)
        return f_peak + (f_end - f_peak) * ((p - 0.35) / 0.65)

    def envelope(p: float) -> float:
        if p < 0.08:                      # quick attack
            return p / 0.08
        if p > 0.55:                      # long soft tail
            x = (1.0 - p) / 0.45
            return x * x
        return 1.0

    samples: list[float] = []
    phase = 0.0
    for i in range(int(RATE * dur)):
        p = i / (RATE * dur)
        t = i / RATE
        # a touch of vibrato keeps it from sounding like a dial-up tone
        freq = freq_at(p) * (1.0 + 0.025 * math.sin(2 * math.pi * 7.0 * t))
        phase += 2 * math.pi * freq / RATE
        value = (math.sin(phase)
                 + 0.45 * math.sin(2 * phase)
                 + 0.18 * math.sin(3 * phase)
                 + 0.06 * math.sin(4 * phase))
        samples.append(gain * envelope(p) * value / 1.69)
    return samples


def write_wav(path: Path, samples: list[float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(RATE)
        wf.writeframes(b"".join(
            struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
            for s in samples))
    print(f"  {path.relative_to(ROOT)}  {len(samples) / RATE:.2f}s")


def main() -> int:
    out = ROOT / "configs" / "noxis" / "airootfs" / "usr" / "share" / "sounds" / "noxis"
    print("noxis sounds")
    write_wav(out / "meow.wav", synth(0.45, 520, 880, 470, 0.50))      # hello!
    write_wav(out / "meow-off.wav", synth(0.55, 700, 560, 330, 0.45))  # goodnight
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
