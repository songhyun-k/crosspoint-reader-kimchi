#pragma once

#include <cstddef>
#include <string>

class Epub {
 public:
  const std::string& getPath() const {
    static const std::string path = "unused.epub";
    return path;
  }
};
