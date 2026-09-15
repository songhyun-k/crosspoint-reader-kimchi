# kimchi font build inputs

The default UI roles share a 10pt **Kimchi UI** bitmap derived from the pinned
Pretendard Regular input (KS X 1001: 2,350 Hangul syllables). The default reader
is the 14pt **KoPubBatang** bitmap derived from KoPub Batang Light, preserving
11,172 Hangul syllables, 4,620 Hanja and the agreed Jamo/punctuation coverage.
Upstream Noto reader families and their sizes remain available. Rare syllables
outside the UI subset are not guaranteed in filenames or titles without an
appropriate SD fallback. Physical display/layout quality needs device testing.

Both bitmaps use the upstream two-bit format and renderer. The UI bitmap is
uncompressed; the body bitmap uses DEFLATE with each uncompressed group capped
at 8,192 bytes instead of the converter's 64 KiB default. This bounds the body's
decompression scratch allocation, not total heap use. Glyph coverage and raster
quality are not reduced by the cap.

From the repository root, with Python packages in a project-local environment:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r lib/EpdFont/scripts/requirements.txt
.venv/bin/python lib/EpdFont/scripts/convert-korean-fonts.py
bash lib/EpdFont/scripts/build-font-ids.sh > src/fontIds.h
.venv/bin/python lib/EpdFont/scripts/build-sd-fonts.py \
  --only KoPubBatang,NotoSansExtended --jobs 2 \
  --output-dir build/kimchi-sd-fonts --manifest
cmake -S test -B build/test -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build/test
KIMCHI_SD_FONT_FIXTURES="$PWD/build/kimchi-sd-fonts" \
  ctest --test-dir build/test --output-on-failure
```

The recorded generation environment is FreeType 2.13.2, freetype-py 2.5.1,
fonttools 4.65.0, PyYAML 6.0.3, zopfli 0.4.3. Source hashes and original font
notices are retained in `lib/EpdFont/builtinFonts/source/`. Install the font
requirements for the optional source/catalog checks; the C++ built-in tests do
not require those packages. Local SD-file tests explicitly skip when fixtures
have not been generated.

`sd-fonts.yaml` retains every original upstream family and adds KoPubBatang at
12/14/16/18pt. Its only available source style is Light, exported as regular;
there are no invented Bold/Italic input files. Do not replace the `.cpfont` v4
reader, web manager, installer, or Text Settings paths with KO's `.epdfont`.

`cpfont_version.py` derives `sd-fonts-m<M>-b<B>` and the kimchi base URL from the
actual manifest/binary versions. The firmware uses the same values through
`src/activities/settings/FontDownloadActivity.h`; regression tests compare the constants. With M=1 and B=4:

`https://github.com/songhyun-k/crosspoint-reader-kimchi/releases/download/sd-fonts-m1-b4/fonts.json`

The local fixture is not a published catalog. To prepare the full release,
remove `--only`, verify every source license and conversion result, then publish
the generated manifest and the flat `<Family>_<pt>.cpfont` files together. Mark
the font release `make_latest=false`, keeping firmware OTA's `latest` separate.
