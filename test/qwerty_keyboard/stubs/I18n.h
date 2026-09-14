#pragma once

#include <cstdint>

// Keyboard policy only needs the name-table enum, not the generated UI strings.
// Keeping just EN/KO also catches accidental dependencies on other UI languages.
enum class Language : uint8_t { EN, KO };
