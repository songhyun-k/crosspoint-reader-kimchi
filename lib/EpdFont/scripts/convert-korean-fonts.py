#!/usr/bin/env python3
"""Generate kimchi fonts with upstream fontconvert.py.

Uses the tracked, hash-pinned source fonts. No network, checkout, publication
or device access is performed.
Adapted from Eunchurn Park's Korean built-in font script and KS X 1001 input.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
BUILTINS = HERE.parent / "builtinFonts"
SOURCES = {
    "KoPub-Batang/KoPub Batang Light.ttf": "b7438d61595fe9584706ab2cb3c9aa2acdc486d61c8299374a30b3e77bc2045f",
    "Pretendard/Pretendard-Regular.ttf": "6d0af5258997aec7354a6e340fc2325ba321c410ca48b3af858c8c3d6e92a324",
}
KOPUB_RANGES = [(0x1100, 0x11FF), (0x3000, 0x303F), (0x3130, 0x318F), (0x4E00, 0x9FFF), (0xAC00, 0xD7A3)]


def ks_x_1001_syllables() -> list[int]:
    points = sorted(ord(bytes([hi, lo]).decode("euc_kr")) for hi in range(0xB0, 0xC9) for lo in range(0xA1, 0xFF))
    if len(set(points)) != 2350 or not all(0xAC00 <= cp <= 0xD7A3 for cp in points):
        raise ValueError("Unexpected KS X 1001 mapping; refusing to change UI coverage")
    return points


def intervals(points: list[int]) -> list[tuple[int, int]]:
    result = []
    for cp in points:
        if result and cp == result[-1][1] + 1:
            result[-1] = (result[-1][0], cp)
        else:
            result.append((cp, cp))
    return result


def convert(name: str, size: int, relative: str, ranges: list[tuple[int, int]], compress: bool = True) -> None:
    target = BUILTINS / f"{name}.h"
    args = [sys.executable, "fontconvert.py", name, str(size), "../builtinFonts/source/" + relative,
            "--2bit"]
    if compress:
        args.extend(["--compress", "--zopfli", "--max-group-bytes", "8192"])
    for start, end in ranges:
        args.extend(["--additional-intervals", f"0x{start:X},0x{end:X}"])
    print(f"Generating {target.name} ({'group DEFLATE' if compress else 'uncompressed'})", flush=True)
    completed = subprocess.run(args, cwd=HERE, check=True, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    text = re.sub(r"^ \* Command used: .*$",
                  " * Command used: python lib/EpdFont/scripts/convert-korean-fonts.py\n"
                  " * Source copyright/licensing: source/KOREAN-FONTS.md and source/korean-font-sources.json.",
                  completed.stdout, count=1, flags=re.MULTILINE)
    temporary = target.with_suffix(".h.tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(target)


def main() -> None:
    for relative, expected in SOURCES.items():
        if hashlib.sha256((BUILTINS / "source" / relative).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Source hash mismatch: {relative}")
    convert("kimchi_batang_14_regular", 14, "KoPub-Batang/KoPub Batang Light.ttf", KOPUB_RANGES)
    convert("kimchi_ui_10_regular", 10, "Pretendard/Pretendard-Regular.ttf",
            [(0x1100, 0x11FF), (0x3130, 0x318F)] + intervals(ks_x_1001_syllables()), compress=False)
    subprocess.run([sys.executable, "verify_compression.py", "../builtinFonts/"], cwd=HERE, check=True)


if __name__ == "__main__":
    main()
