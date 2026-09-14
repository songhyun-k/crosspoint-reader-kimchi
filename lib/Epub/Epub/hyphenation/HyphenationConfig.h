#pragma once

// Unconfigured host tools retain upstream's complete registry. Firmware selects
// zero in platformio.ini so none of the generated language tries are linked.
#ifndef CP_HYPHENATION_LANGS
#define CP_HYPHENATION_LANGS 1
#endif

constexpr bool effectiveHyphenationEnabled(const bool requested) { return CP_HYPHENATION_LANGS != 0 && requested; }
