#pragma once
#include <HalStorage.h>

#include <string>

class CssParser;
// Book metadata fixture; extraction, parsing and cache I/O use production code.
class Epub {
 public:
  std::string archivePath = "images.epub";
  std::string spineHref = "chapter.xhtml";
  struct Spine {
    std::string href = "chapter.xhtml";
  };
  struct Toc {
    int spineIndex = 0;
    std::string anchor;
  };
  std::string getCachePath() const { return "cache"; }
  const std::string& getPath() const { return archivePath; }
  std::string getLanguage() const { return "ko"; }
  Spine getSpineItem(int) const { return {spineHref}; }
  Toc getTocItem(int) const { return {}; }
  int getTocIndexForSpineIndex(int) const { return -1; }
  int getTocItemsCount() const { return 0; }
  CssParser* getCssParser() const { return nullptr; }
};
