#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  // HalFile::read() returns a negative int on error. Treat it as end-of-stream
  // rather than letting the negative-to-size_t conversion underflow fileRemaining
  // and report a huge bytesRead, which would have the inflate library read past
  // the end of readBuf.
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

struct ZipFile::ReadState {
  enum class Phase : uint8_t { Metadata, Directory, LocalHeader, Data };
  ScopedOpenClose archive;
  const char* filename;
  const size_t chunkSize;
  const bool allowEarlyStop;
  bool borrowBuildScratch = false;
  Phase phase = Phase::Metadata;
  FileStatSlim stat = {};
  uint32_t directoryStart = 0;
  bool directoryWrapped = false;
  std::unique_ptr<uint8_t[]> input;
  std::unique_ptr<uint8_t[]> output;
  InflateStream inflate;
  ZipInflateCtx context;
  size_t outputUsed = 0;
  size_t totalProduced = 0;

  ReadState(ZipFile& zip, const char* filename, const size_t chunkSize, const bool allowEarlyStop)
      : archive(zip), filename(filename), chunkSize(chunkSize), allowEarlyStop(allowEarlyStop) {}
};

ZipFile::ZipFile(const std::string& filePath) : filePath(filePath) {}
ZipFile::~ZipFile() = default;

bool ZipFile::readCentralDirectoryEntry(FileStatSlim& stat, char* name, const size_t nameCapacity) {
  uint8_t header[46];
  if (file.read(header, sizeof(header)) != sizeof(header) || memcmp(header, "PK\x01\x02", 4) != 0) return false;
  uint16_t nameLen, extraLen, commentLen;
  memcpy(&stat.method, header + 10, sizeof(stat.method));
  memcpy(&stat.compressedSize, header + 20, sizeof(stat.compressedSize));
  memcpy(&stat.uncompressedSize, header + 24, sizeof(stat.uncompressedSize));
  memcpy(&nameLen, header + 28, sizeof(nameLen));
  memcpy(&extraLen, header + 30, sizeof(extraLen));
  memcpy(&commentLen, header + 32, sizeof(commentLen));
  memcpy(&stat.localHeaderOffset, header + 42, sizeof(stat.localHeaderOffset));
  if (nameLen < nameCapacity) {
    if (file.read(name, nameLen) != nameLen) return false;
    name[nameLen] = '\0';
  } else {
    name[0] = '\0';
    if (!file.seekCur(nameLen)) return false;
  }
  return file.seekCur(static_cast<uint32_t>(extraLen) + commentLen);
}

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (!readCentralDirectoryEntry(*fileStat, itemName, sizeof(itemName))) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    if (strcmp(itemName, filename) == 0) {
      lastCentralDirPos = file.position();
      lastCentralDirPosValid = true;
      found = true;
      break;
    }
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = makeUniqueNoThrowForOverwrite<uint8_t[]>(scanRange);
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  if (file.read(buffer.get(), scanRange) != scanRange) {
    LOG_ERR("ZIP", "Failed to read EOCD scan buffer");
    return false;
  }

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    uint32_t value;
    memcpy(&value, buffer.get() + i, sizeof(value));
    if (value == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  memcpy(&zipDetails.totalEntries, buffer.get() + foundOffset + 10, sizeof(zipDetails.totalEntries));
  memcpy(&zipDetails.centralDirOffset, buffer.get() + foundOffset + 16, sizeof(zipDetails.centralDirOffset));
  zipDetails.isSet = true;

  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      free(data);
      return nullptr;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = 1024;

    // One-shot mode: `data` holds the entire output, so back-references
    // resolve inside it and no 32KB window is allocated.
    InflateStream inflate;
    if (!inflate.init(false)) {
      LOG_ERR("ZIP", "Failed to init inflate stream");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    inflate.setFill(zipFillCallback, &ctx);

    if (!inflate.read(data, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    free(fileReadBuffer);

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::beginReadFileToStream(const char* filename, const size_t chunkSize, const bool allowEarlyStop) {
  if (readState || !filename || !*filename || chunkSize == 0) {
    LOG_ERR("ZIP", "Invalid or overlapping stream request");
    return false;
  }
  readState = makeUniqueNoThrow<ReadState>(*this, filename, chunkSize, allowEarlyStop);
  if (!readState || !readState->archive) {
    LOG_ERR("ZIP", "Failed to open stream");
    readState.reset();
    return false;
  }
  return true;
}

ZipFile::ReadStatus ZipFile::readSome(Print& out) {
  if (!readState) {
    LOG_ERR("ZIP", "No active stream");
    return ReadStatus::Error;
  }
  const auto finish = [this](const ReadStatus status) {
    readState.reset();
    return status;
  };
  auto& work = *readState;
  switch (work.phase) {
    case ReadState::Phase::Metadata: {
      work.input = makeUniqueNoThrowForOverwrite<uint8_t[]>(std::max<size_t>(256, work.chunkSize));
      if (!work.input) {
        LOG_ERR("ZIP", "OOM: stream input");
        return finish(ReadStatus::Error);
      }
      if (!fileStatSlimCache.empty()) {
        const auto entry = fileStatSlimCache.find(work.filename);
        if (entry == fileStatSlimCache.end()) {
          LOG_ERR("ZIP", "Entry not found: %s", work.filename);
          return finish(ReadStatus::Error);
        }
        work.stat = entry->second;
        work.phase = ReadState::Phase::LocalHeader;
      } else {
        if (!loadZipDetails()) return finish(ReadStatus::Error);
        work.directoryStart = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
        if (!file.seek(work.directoryStart)) {
          LOG_ERR("ZIP", "Invalid central directory offset");
          return finish(ReadStatus::Error);
        }
        work.phase = ReadState::Phase::Directory;
      }
      return ReadStatus::More;
    }
    case ReadState::Phase::Directory: {
      const uint32_t position = file.position();
      auto* name = reinterpret_cast<char*>(work.input.get());
      if ((work.directoryWrapped && position >= work.directoryStart) ||
          !readCentralDirectoryEntry(work.stat, name, 256)) {
        if (!work.directoryWrapped && work.directoryStart != zipDetails.centralDirOffset) {
          work.directoryWrapped = true;
          if (file.seek(zipDetails.centralDirOffset)) return ReadStatus::More;
        }
        LOG_ERR("ZIP", "Entry not found: %s", work.filename);
        return finish(ReadStatus::Error);
      }
      if (strcmp(work.filename, name) == 0) {
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        work.phase = ReadState::Phase::LocalHeader;
      }
      return ReadStatus::More;
    }
    case ReadState::Phase::LocalHeader: {
      if (work.stat.method != ZIP_METHOD_STORED && work.stat.method != ZIP_METHOD_DEFLATED) {
        LOG_ERR("ZIP", "Unsupported compression method");
        return finish(ReadStatus::Error);
      }
      const long offset = getDataOffset(work.stat);
      if (offset < 0 || !file.seek(offset)) {
        LOG_ERR("ZIP", "Invalid entry offset");
        return finish(ReadStatus::Error);
      }
      if (work.stat.method == ZIP_METHOD_DEFLATED) {
        work.output = makeUniqueNoThrowForOverwrite<uint8_t[]>(work.chunkSize);
        if (!work.output || !work.inflate.init(true, work.borrowBuildScratch)) {
          LOG_ERR("ZIP", "OOM: stream inflate");
          return finish(ReadStatus::Error);
        }
        work.context = {&file, work.stat.compressedSize, work.input.get(), work.chunkSize};
        work.inflate.setFill(zipFillCallback, &work.context);
      }
      work.phase = ReadState::Phase::Data;
      return ReadStatus::More;
    }
    case ReadState::Phase::Data:
      break;
  }

  if (work.stat.method == ZIP_METHOD_STORED) {
    const size_t remaining = work.stat.uncompressedSize - work.totalProduced;
    if (remaining == 0) return finish(ReadStatus::Done);
    const int count = file.read(work.input.get(), std::min(remaining, work.chunkSize));
    if (count <= 0) {
      LOG_ERR("ZIP", "Failed to read stored entry");
      return finish(ReadStatus::Error);
    }
    work.totalProduced += count;
    if (out.write(work.input.get(), count) != static_cast<size_t>(count)) {
      if (work.allowEarlyStop) return finish(ReadStatus::Done);
      LOG_ERR("ZIP", "Failed to write all output bytes to stream");
      return finish(ReadStatus::Error);
    }
    return work.totalProduced == work.stat.uncompressedSize ? finish(ReadStatus::Done) : ReadStatus::More;
  }

  size_t produced = 0;
  const auto status =
      work.inflate.readAtMost(work.output.get() + work.outputUsed, work.chunkSize - work.outputUsed, &produced, 1);
  work.outputUsed += produced;
  work.totalProduced += produced;
  if (work.totalProduced > work.stat.uncompressedSize) {
    LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %lu)", work.totalProduced,
            static_cast<unsigned long>(work.stat.uncompressedSize));
    return finish(ReadStatus::Error);
  }
  // Keep the original output chunk boundaries even when a call only refills input.
  if (work.outputUsed > 0 && (work.outputUsed == work.chunkSize || status != InflateStream::Status::Ok)) {
    if (out.write(work.output.get(), work.outputUsed) != work.outputUsed) {
      if (work.allowEarlyStop) return finish(ReadStatus::Done);
      LOG_ERR("ZIP", "Failed to write all output bytes to stream");
      return finish(ReadStatus::Error);
    }
    work.outputUsed = 0;
  }
  if (status == InflateStream::Status::Error) {
    LOG_ERR("ZIP", "Decompression failed");
    return finish(ReadStatus::Error);
  }
  if (status == InflateStream::Status::Done) {
    if (work.totalProduced != work.stat.uncompressedSize) {
      LOG_ERR("ZIP", "Decompressed size mismatch (expected %lu, got %zu)",
              static_cast<unsigned long>(work.stat.uncompressedSize), work.totalProduced);
      return finish(ReadStatus::Error);
    }
    return finish(ReadStatus::Done);
  }
  return ReadStatus::More;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  if (!beginReadFileToStream(filename, chunkSize, allowEarlyStop)) return false;
  // This drain cannot outlive its caller's framebuffer loan.
  readState->borrowBuildScratch = true;
  ReadStatus status;
  do {
    status = readSome(out);
  } while (status == ReadStatus::More);
  return status == ReadStatus::Done;
}
