#include "KimchiRelease.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace kimchi_release {
namespace {
bool takeNumber(std::string_view& text, uint32_t& value) {
  size_t count = 0;
  value = 0;
  while (count < text.size() && text[count] >= '0' && text[count] <= '9') {
    const uint32_t digit = text[count] - '0';
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
    ++count;
  }
  if (count == 0 || (count > 1 && text.front() == '0')) return false;
  text.remove_prefix(count);
  return true;
}

bool takePrefix(std::string_view& text, const std::string_view prefix) {
  if (text.substr(0, prefix.size()) != prefix) return false;
  text.remove_prefix(prefix.size());
  return true;
}
}  // namespace

bool parseVersion(std::string_view text, Version& result) {
  if (!text.empty() && text.front() == 'v') text.remove_prefix(1);
  Version parsed;
  if (!takeNumber(text, parsed.numbers[0]) || !takePrefix(text, ".") || !takeNumber(text, parsed.numbers[1]) ||
      !takePrefix(text, ".") || !takeNumber(text, parsed.numbers[2]) || !takePrefix(text, "-kimchi.") ||
      !takeNumber(text, parsed.numbers[3]) || parsed.numbers[3] == 0 || !text.empty()) {
    return false;
  }
  result = parsed;
  return true;
}

bool isNewer(const std::string_view candidate, const std::string_view current) {
  Version next, installed;
  return parseVersion(candidate, next) && parseVersion(current, installed) && next.numbers > installed.numbers;
}

bool assetName(const std::string_view board, char* output, const size_t capacity) {
  if (!output || capacity == 0) return false;
  output[0] = '\0';
  if (board.empty() || board.size() > 23) return false;
  if (!std::all_of(board.begin(), board.end(),
                   [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
    return false;
  const int length = board == "x4" ? std::snprintf(output, capacity, "firmware.bin")
                                   : std::snprintf(output, capacity, "firmware-%.*s.bin",
                                                   static_cast<int>(board.size()), board.data());
  if (length < 0 || static_cast<size_t>(length) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

bool matchesAssetUrl(std::string_view url, const std::string_view tag, const std::string_view asset) {
  // Compare the original tag (including optional v); only version comparison
  // normalises that prefix. Redirects still use HttpDownloader's TLS checks.
  return takePrefix(url, DOWNLOAD_PREFIX) && takePrefix(url, tag) && takePrefix(url, "/") && url == asset;
}
}  // namespace kimchi_release
