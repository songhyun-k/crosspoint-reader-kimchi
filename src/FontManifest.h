#pragma once

#include <SdCardFont.h>

// Keep these versions in sync with lib/EpdFont/scripts/cpfont_version.py.
#define FONTS_MANIFEST_VERSION 1
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL                                                                                            \
  "https://github.com/songhyun-k/crosspoint-reader-kimchi/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif
