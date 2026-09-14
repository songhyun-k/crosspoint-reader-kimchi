#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace storage_test {
inline std::unordered_map<std::string, std::vector<uint8_t>> files;
inline size_t shortReadAt = std::numeric_limits<size_t>::max();
inline bool negativeRead = false;
inline size_t reads = 0;
inline size_t largestRead = 0;
inline size_t openHandles = 0;
inline void reset() {
  files.clear();
  shortReadAt = std::numeric_limits<size_t>::max();
  negativeRead = false;
  reads = largestRead = 0;
}
}  // namespace storage_test

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const std::string& path) {
    close();
    const auto it = storage_test::files.find(path);
    if (it == storage_test::files.end()) return false;
    bytes_ = &it->second;
    ++storage_test::openHandles;
    return true;
  }
  int read(void* dest, size_t count) {
    ++storage_test::reads;
    storage_test::largestRead = std::max(storage_test::largestRead, count);
    if (!bytes_ || offset_ > bytes_->size()) return -1;
    if (offset_ == storage_test::shortReadAt) {
      if (storage_test::negativeRead) return -1;
      if (count > 0) --count;
    }
    count = std::min(count, bytes_->size() - offset_);
    if (count > 0) std::memcpy(dest, bytes_->data() + offset_, count);
    offset_ += count;
    return static_cast<int>(count);
  }
  size_t write(const void* source, const size_t count) {
    if (!bytes_) return 0;
    bytes_->resize(std::max(bytes_->size(), offset_ + count));
    if (count) std::memcpy(bytes_->data() + offset_, source, count);
    offset_ += count;
    return count;
  }
  size_t write(const uint8_t byte) { return write(&byte, 1); }
  size_t position() const { return offset_; }
  size_t size() const { return fileSize64(); }
  size_t fileSize() const { return size(); }
  int available() const { return static_cast<int>(size() - offset_); }
  bool flush() const { return isOpen(); }
  bool seekCur(const size_t bytes) { return seek64(offset_ + bytes); }
  bool seek64(uint64_t offset) {
    if (!bytes_ || offset > bytes_->size()) return false;
    offset_ = static_cast<size_t>(offset);
    return true;
  }
  bool seek(uint32_t offset) { return seek64(offset); }
  bool seekSet(uint32_t offset) { return seek64(offset); }
  uint64_t fileSize64() const { return bytes_ ? bytes_->size() : 0; }
  bool isOpen() const { return bytes_ != nullptr; }
  explicit operator bool() const { return isOpen(); }
  bool close() {
    const bool wasOpen = bytes_ != nullptr;
    if (bytes_) --storage_test::openHandles;
    bytes_ = nullptr;
    offset_ = 0;
    return wasOpen;
  }

 private:
  std::vector<uint8_t>* bytes_ = nullptr;
  size_t offset_ = 0;
};

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    storage_test::files[path].clear();
    return file.open(path);
  }
  bool exists(const std::string& path) const { return storage_test::files.contains(path); }
  bool mkdir(const char*) { return true; }
  bool remove(const std::string& path) { return storage_test::files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    auto node = storage_test::files.extract(from);
    if (node.empty()) return false;
    node.key() = to;
    return storage_test::files.insert(std::move(node)).inserted;
  }
};
inline HalStorage Storage;

struct TestEsp {
  uint32_t freeHeap = 1024 * 1024;
  uint32_t maxAlloc = 1024 * 1024;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return maxAlloc; }
  void restart() { std::abort(); }
};
inline TestEsp ESP;
inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
