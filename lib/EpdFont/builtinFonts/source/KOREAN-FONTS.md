# Korean font source provenance

These inputs are copied without modification from the user-provided
`crosspoint-reader-ko` Git object `84a39194dfce1ebd772ac9163df0a59daa0d72dc`.
`convert-korean-fonts.py --ko-repo <path>` verifies their SHA256 before writing
them and refuses to overwrite different local inputs. The generated
`korean-font-sources.json` preserves copyright, author, family/style, version
and embedded licensing metadata. Fonts are not relicensed by this repository's
GPL source-code license.

## Pretendard

Original font: Pretendard Regular, Version 1.309. Copyright 2023 Kil Hyung-jin;
the project's OFL notice also retains its original 2021 copyright. SIL Open
Font License 1.1, Reserved Font Name Pretendard. The upstream license is retained
in `Pretendard/OFL.txt`. The reduced bitmap font's primary name is **Kimchi UI**;
“Pretendard” identifies its source, not an endorsement. The source credits for
Inter, Noto Sans CJK/Source Han Sans and M PLUS 1p remain in the source name table
and the generated provenance JSON.

The bitmap includes exactly the agreed KS X 1001 2,350 Hangul syllables,
alongside the existing converter's non-Hangul coverage and the source's Jamo.

Primary license: https://github.com/orioncactus/pretendard/blob/v1.3.9/LICENSE

## KoPub Batang — publication clearance remains open

The selected file is **KoPub Batang Light, Version 2.1.1; Build Release**,
copyright **2018 Korea Publisher Society; designed by FONTRIX Inc.** Its name
table has no license text or URL. The pinned KO repository has no KoPub license
file. The converter retains all 11,172 precomposed Hangul syllables, the source's
4,620 Hanja and the agreed Jamo/punctuation intervals. No Bold source is present.

Do not infer this file's redistribution terms from Google's separately supplied
2014 KoPub font or from the repository's GPL license. A primary-source license
covering this exact 2018 input and bitmap/cpfont conversion still needs to be
confirmed **before remote publication**. Local extraction, conversion and tests
use the user-supplied source; no public font asset is being published here.

Sources checked on 2026-09-15:

- Publisher download page: https://www.kopus.org/biz-electronic-font2/
  (returned a hosting-provider 403 page during this run).
- Google Fonts' distinct 2014 license:
  https://github.com/google/fonts/blob/main/ofl/kopubbatang/OFL.txt
  (not treated as proof for the selected 2018 input).

The converted family is named **Kimchi Batang**; catalogue descriptions identify
KoPub Batang Light as its source. The name alone does not resolve the outstanding
permission question. Preserve these notices and any subsequently verified license
beside both firmware and SD-font distribution assets.
