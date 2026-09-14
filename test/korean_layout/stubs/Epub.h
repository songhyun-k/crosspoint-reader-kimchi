#pragma once
#include <HalStorage.h>

#include <string>

class CssParser;
// ZIP extraction and book metadata are outside these layout/cache tests. The
// section consumes a real cached XHTML file through the memory HalStorage.
class Epub {
 public:
  struct Spine {
    std::string href = "chapter.xhtml";
  };
  struct Toc {
    int spineIndex = 0;
    std::string anchor;
  };
  std::string getCachePath() const { return "cache"; }
  std::string getLanguage() const { return "ko"; }
  Spine getSpineItem(int) const { return {}; }
  Toc getTocItem(int) const { return {}; }
  int getTocIndexForSpineIndex(int) const { return -1; }
  int getTocItemsCount() const { return 0; }
  CssParser* getCssParser() const { return nullptr; }
  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output&, size_t, bool = false) const {
    return false;
  }
};
