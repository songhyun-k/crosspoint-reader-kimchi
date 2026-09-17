#include <BuildScratch.h>
#include <MinizConfig.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "test/xtc_memory/BinaryFixture.h"

namespace {
const std::string archivePath = "images.epub";
constexpr const char* IMAGE = "OEBPS/images/scaling_test.jpg";

class ZipStreamingTest : public testing::Test {
 protected:
  void SetUp() override {
    storage_test::reset();
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / "epubs/test_jpeg_images.epub",
                        std::ios::binary);
    ASSERT_TRUE(input);
    storage_test::files[archivePath] = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};
}  // namespace

TEST_F(ZipStreamingTest, ResumesStoredAndDeflatedEntriesWithoutBorrowingTheDisplay) {
  struct Entry {
    const char* name;
    size_t size;
    uint32_t crc;
    size_t chunk;
  };
  for (const auto& entry : {Entry{"mimetype", 20, 0x2cab616f, 8}, Entry{IMAGE, 144951, 0x65c979ee, 1024}}) {
    SCOPED_TRACE(entry.name);
    HalFile output;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "output", output));
    ASSERT_TRUE(ZipFile(archivePath).readFileToStream(entry.name, output, entry.chunk));
    const auto expected = storage_test::files.at("output");
    ASSERT_EQ(expected.size(), entry.size);
    ASSERT_EQ(mz_crc32(0, expected.data(), expected.size()), entry.crc);
    const size_t reads = storage_test::reads;
    const size_t readBytes = storage_test::readBytes;
    const size_t writes = storage_test::writes;
    const size_t seeks = storage_test::seeks;
    output.close();

    storage_test::reads = storage_test::readBytes = storage_test::writes = storage_test::seeks = 0;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "output", output));
    // A caller may restore/reuse the framebuffer after any step.
    std::vector<uint8_t> framebuffer(48000);
    buildscratch::lend(framebuffer.data(), framebuffer.size());
    struct Reclaim {
      ~Reclaim() { buildscratch::reclaim(); }
    } reclaim;
    ZipFile zip(archivePath);
    ASSERT_TRUE(zip.beginReadFileToStream(entry.name, entry.chunk));
    auto status = ZipFile::ReadStatus::More;
    for (int step = 0; step < 2000 && status == ZipFile::ReadStatus::More; ++step) {
      const size_t beforeReads = storage_test::reads;
      const size_t beforeWrites = storage_test::writes;
      status = zip.readSome(output);
      EXPECT_LE(storage_test::reads - beforeReads, 2u);  // directory header + name; data uses <= 1
      EXPECT_LE(storage_test::writes - beforeWrites, 1u);
      const auto* loan = buildscratch::claim(framebuffer.size());
      ASSERT_EQ(loan, framebuffer.data());
      buildscratch::release(loan);
      std::fill(framebuffer.begin(), framebuffer.end(), 0xa5);
    }
    ASSERT_EQ(status, ZipFile::ReadStatus::Done);
    EXPECT_FALSE(zip.isOpen());
    EXPECT_EQ(storage_test::files.at("output"), expected);
    EXPECT_EQ(storage_test::reads, reads);
    EXPECT_EQ(storage_test::readBytes, readBytes);
    EXPECT_EQ(storage_test::writes, writes);
    EXPECT_EQ(storage_test::seeks, seeks);
    output.close();
    EXPECT_EQ(storage_test::openHandles, 0u);
    storage_test::reads = storage_test::readBytes = storage_test::writes = storage_test::seeks = 0;
  }
}

TEST_F(ZipStreamingTest, CancellationReadFailureAndEarlyStopReleaseTheOwnedStream) {
  for (const bool deflated : {false, true}) {
    SCOPED_TRACE(deflated);
    HalFile output;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "output", output));
    auto zip = std::make_unique<ZipFile>(archivePath);
    ASSERT_TRUE(zip->beginReadFileToStream(deflated ? IMAGE : "mimetype", 8));
    for (int step = 0; step < 1000 && output.size() == 0; ++step) {
      ASSERT_EQ(zip->readSome(output), ZipFile::ReadStatus::More);
    }
    ASSERT_GT(output.size(), 0u);
    EXPECT_EQ(storage_test::openHandles, 2u);
    zip.reset();
    EXPECT_EQ(storage_test::openHandles, 1u);
    output.close();
  }

  for (const bool earlyStop : {false, true}) {
    SCOPED_TRACE(earlyStop);
    HalFile output;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "output", output));
    storage_test::shortWriteAt = 0;
    EXPECT_EQ(ZipFile(archivePath).readFileToStream(IMAGE, output, 1024, earlyStop), earlyStop);
    EXPECT_EQ(output.size(), 1023u);
    EXPECT_EQ(storage_test::openHandles, 1u);
    storage_test::shortWriteAt = std::numeric_limits<size_t>::max();
  }

  HalFile output;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "output", output));
  // Payload starts in the fixed EPUB fixture: local header plus entry name/extra.
  for (const size_t offset : {38u, 88241u}) {
    storage_test::shortReadAt = offset;
    storage_test::negativeRead = true;
    EXPECT_FALSE(ZipFile(archivePath).readFileToStream(offset == 38 ? "mimetype" : IMAGE, output, 8));
    EXPECT_EQ(output.size(), 0u);
    EXPECT_EQ(storage_test::openHandles, 1u);
  }
  storage_test::shortReadAt = std::numeric_limits<size_t>::max();
  storage_test::negativeRead = false;
  auto& bytes = storage_test::files.at(archivePath);
  const std::string name = IMAGE;
  const auto centralName = std::find_end(bytes.begin(), bytes.end(), name.begin(), name.end());
  ASSERT_NE(centralName, bytes.end());
  const size_t header = static_cast<size_t>(centralName - bytes.begin()) - 46;
  ASSERT_EQ(bytes.at(header), 'P');
  ASSERT_EQ(bytes.at(header + 2), 1);
  for (const uint32_t declaredSize : {144950u, 144952u}) {
    binary_fixture::put(bytes, header + 24, declaredSize, 4);
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "output", output));
    EXPECT_FALSE(ZipFile(archivePath).readFileToStream(IMAGE, output, 1024));
    EXPECT_EQ(storage_test::openHandles, 1u);
  }
}
