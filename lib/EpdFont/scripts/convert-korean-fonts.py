#!/usr/bin/env python3
"""Generate kimchi fonts with upstream fontconvert.py and its DEFLATE format.

Source extraction only reads the fixed object in an explicitly supplied KO
repository. No network, checkout, publication or device access is performed.
Adapted from Eunchurn Park's Korean built-in font script and KS X 1001 input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
BUILTINS = HERE.parent / "builtinFonts"
KO_SHA = "84a39194dfce1ebd772ac9163df0a59daa0d72dc"
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


def prepare_sources(ko_repo: Path | None) -> None:
    for relative, expected in SOURCES.items():
        target = BUILTINS / "source" / relative
        if not target.exists():
            if ko_repo is None:
                raise ValueError(f"Missing {target}; supply --ko-repo for the pinned input")
            blob_path = "lib/EpdFont/builtinFonts/source/" + relative
            raw = subprocess.run(
                ["git", "-C", str(ko_repo), "show", f"{KO_SHA}:{blob_path}"],
                check=True, capture_output=True,
            ).stdout
            if hashlib.sha256(raw).hexdigest() != expected:
                raise ValueError(f"Pinned source hash mismatch: {relative}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
        elif hashlib.sha256(target.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Existing source differs; not overwriting: {target}")


def source_metadata() -> dict:
    from fontTools.ttLib import TTFont
    import freetype

    result = {"koCommit": KO_SHA, "freeType": ".".join(map(str, freetype.version())), "fonts": {}}
    for relative, expected in SOURCES.items():
        font = TTFont(BUILTINS / "source" / relative)
        cmap = font.getBestCmap()
        names = sorted({f"nameID {n.nameID}: {n.toUnicode()}" for n in font["name"].names
                        if n.nameID in (0, 1, 2, 5, 8, 9, 13, 14)})
        item = {"sha256": expected, "sourceNamesAndNotices": names,
                "hangulSyllables": sum(0xAC00 <= cp <= 0xD7A3 for cp in cmap),
                "hanja": sum(0x4E00 <= cp <= 0x9FFF for cp in cmap)}
        if relative.startswith("KoPub"):
            if item["hangulSyllables"] != 11172 or item["hanja"] != 4620:
                raise ValueError(f"KoPub coverage changed: {item}")
        elif not set(ks_x_1001_syllables()).issubset(cmap):
            raise ValueError("Pretendard input is missing agreed UI syllables")
        result["fonts"][relative] = item
        font.close()
    return result


def convert(name: str, size: int, relative: str, ranges: list[tuple[int, int]]) -> None:
    target = BUILTINS / f"{name}.h"
    args = [sys.executable, "fontconvert.py", name, str(size), "../builtinFonts/source/" + relative,
            "--2bit", "--compress", "--zopfli", "--max-group-bytes", "8192"]
    for start, end in ranges:
        args.extend(["--additional-intervals", f"0x{start:X},0x{end:X}"])
    print(f"Generating {target.name} with existing group DEFLATE", flush=True)
    completed = subprocess.run(args, cwd=HERE, check=True, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    text = re.sub(r"^ \* Command used: .*$",
                  " * Command used: python lib/EpdFont/scripts/convert-korean-fonts.py\n"
                  " * Source copyright/licensing: source/KOREAN-FONTS.md and source/korean-font-sources.json.",
                  completed.stdout, count=1, flags=re.MULTILINE)
    temporary = target.with_suffix(".h.tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(target)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ko-repo", type=Path, help="Read only the fixed SHA from this existing KO repository")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    prepare_sources(args.ko_repo)
    metadata = source_metadata()
    (BUILTINS / "source/korean-font-sources.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, ensure_ascii=False, indent=2), flush=True)
    if args.prepare_only:
        return
    convert("kimchi_batang_14_regular", 14, "KoPub-Batang/KoPub Batang Light.ttf", KOPUB_RANGES)
    convert("kimchi_ui_10_regular", 10, "Pretendard/Pretendard-Regular.ttf",
            [(0x1100, 0x11FF), (0x3130, 0x318F)] + intervals(ks_x_1001_syllables()))
    subprocess.run([sys.executable, "verify_compression.py", "../builtinFonts/"], cwd=HERE, check=True)


if __name__ == "__main__":
    main()
