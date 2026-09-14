#include <HalStorage.h>
#include <Xtc/XtcBitmap.h>
#include <Xtc/XtcParser.h>
#include <gtest/gtest.h>

#include <limits>
#include <new>
#include <utility>

#include "BinaryFixture.h"

namespace allocation_test {
bool active = false;
unsigned failures = 0;
unsigned attempts = 0;
size_t largest = 0;

struct ArrayFaultScope {
  explicit ArrayFaultScope(unsigned count) {
    failures = count;
    attempts = 0;
    largest = 0;
    active = true;
  }
  ~ArrayFaultScope() { active = false; }
};
}  // namespace allocation_test

// Fault injection is confined to this host executable. Production uses its
// real nothrow array allocator, with no test switch or alternate allocation API.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (allocation_test::active) {
    ++allocation_test::attempts;
    allocation_test::largest = std::max(allocation_test::largest, size);
    if (allocation_test::failures > 0) {
      --allocation_test::failures;
      return nullptr;
    }
  }
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

namespace {
constexpr size_t TABLE_OFFSET = sizeof(xtc::XtcHeader);

std::vector<uint8_t> monoBitmap(const uint16_t width, const uint16_t height) {
  const size_t rowBytes = (static_cast<size_t>(width) + 7) / 8;
  // Black padding bits deliberately test that no pixel beyond width is drawn.
  std::vector<uint8_t> bytes(rowBytes * height, 0);
  for (uint16_t y = 0; y < height; ++y) {
    for (uint16_t x = 0; x < width; ++x) {
      if ((x * 3 + y * 5) % 7 >= 3) bytes[y * rowBytes + x / 8] |= 0x80 >> (x % 8);
    }
  }
  return bytes;
}

std::vector<uint8_t> makeXtc(const std::vector<std::pair<uint16_t, uint16_t>>& sizes, const uint8_t depth = 1) {
  xtc::XtcHeader header{};
  header.magic = depth == 1 ? xtc::XTC_MAGIC : xtc::XTCH_MAGIC;
  header.versionMajor = 1;
  header.pageCount = static_cast<uint16_t>(sizes.size());
  header.pageTableOffset = TABLE_OFFSET;
  header.dataOffset = TABLE_OFFSET + sizeof(xtc::PageTableEntry) * sizes.size();
  std::vector<uint8_t> bytes;
  binary_fixture::append(bytes, header);
  bytes.resize(static_cast<size_t>(header.dataOffset));
  for (size_t index = 0; index < sizes.size(); ++index) {
    const auto [width, height] = sizes[index];
    auto bitmap = depth == 1 ? monoBitmap(width, height)
                             : std::vector<uint8_t>(((static_cast<size_t>(width) * height + 7) / 8) * 2, 0xA6);
    xtc::PageTableEntry entry{bytes.size(), static_cast<uint32_t>(sizeof(xtc::XtgPageHeader) + bitmap.size()), width,
                              height};
    std::memcpy(bytes.data() + TABLE_OFFSET + index * sizeof(entry), &entry, sizeof(entry));
    xtc::XtgPageHeader page{};
    page.magic = depth == 1 ? xtc::XTG_MAGIC : xtc::XTH_MAGIC;
    page.width = width;
    page.height = height;
    page.dataSize = bitmap.size();
    binary_fixture::append(bytes, page);
    bytes.insert(bytes.end(), bitmap.begin(), bitmap.end());
  }
  return bytes;
}

class XtcStreamingTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(storage_test::openHandles, 0u);
    storage_test::reset();
  }
  void TearDown() override { EXPECT_EQ(storage_test::openHandles, 0u); }
};

constexpr size_t FIRST_PAGE = TABLE_OFFSET + sizeof(xtc::PageTableEntry);
constexpr size_t FIRST_BITMAP = FIRST_PAGE + sizeof(xtc::XtgPageHeader);
}  // namespace

TEST_F(XtcStreamingTest, FullX4PageUsesOne960ByteAllocationAndFiftyBands) {
  storage_test::files["book.xtc"] = makeXtc({{480, 800}});
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
  const auto expected = monoBitmap(480, 800);
  size_t total = 0;
  unsigned bands = 0;
  xtc::XtcError result;
  {
    allocation_test::ArrayFaultScope allocations(0);
    result = parser.loadPageStreaming(
        0,
        [&](const uint8_t* data, size_t count, size_t offset) {
          EXPECT_EQ(offset, total);
          EXPECT_EQ(count, 960u);
          EXPECT_EQ(std::memcmp(data, expected.data() + offset, count), 0);
          total += count;
          ++bands;
        },
        60 * xtc::MONO_STREAM_ROWS);
  }
  EXPECT_EQ(result, xtc::XtcError::OK);
  EXPECT_EQ(total, 48000u);
  EXPECT_EQ(bands, 50u);
  EXPECT_EQ(allocation_test::attempts, 1u);
  EXPECT_EQ(allocation_test::largest, 960u);
  EXPECT_LE(storage_test::largestRead, 960u);
  EXPECT_EQ(storage_test::openHandles, 0u);
}

TEST_F(XtcStreamingTest, OddWidthAndFinalThreeRowsMatchEveryLegacyLogicalPixel) {
  constexpr uint16_t width = 13, height = 35;
  storage_test::files["book.xtc"] = makeXtc({{width, height}});
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
  std::vector<uint8_t> pixels(width * height, 0);
  std::vector<size_t> bands;
  ASSERT_EQ(parser.loadPageStreaming(
                0,
                [&](const uint8_t* data, size_t count, size_t offset) {
                  bands.push_back(count);
                  xtc::drawMonochromeBand(data, count, offset, width, [&](uint16_t x, uint16_t y) {
                    ASSERT_LT(x, width);
                    ASSERT_LT(y, height);
                    ++pixels[y * width + x];
                  });
                },
                2 * xtc::MONO_STREAM_ROWS),
            xtc::XtcError::OK);
  EXPECT_EQ(bands, (std::vector<size_t>{32, 32, 6}));
  for (uint16_t y = 0; y < height; ++y) {
    for (uint16_t x = 0; x < width; ++x) {
      EXPECT_EQ(pixels[y * width + x], (x * 3 + y * 5) % 7 < 3 ? 1 : 0) << x << "," << y;
    }
  }
}

TEST_F(XtcStreamingTest, EachPageUsesItsOwnDimensions) {
  storage_test::files["book.xtc"] = makeXtc({{480, 800}, {13, 35}});
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
  xtc::PageInfo page{};
  ASSERT_TRUE(parser.getPageInfo(1, page));
  EXPECT_EQ(storage_test::openHandles, 0u);  // release SD buffering before allocating a page
  EXPECT_EQ(page.width, 13);
  EXPECT_EQ(page.height, 35);
  const auto expected = monoBitmap(page.width, page.height);
  size_t total = 0;
  ASSERT_EQ(parser.loadPageStreaming(
                1,
                [&](const uint8_t* data, size_t count, size_t offset) {
                  EXPECT_EQ(std::memcmp(data, expected.data() + offset, count), 0);
                  total += count;
                },
                32),
            xtc::XtcError::OK);
  EXPECT_EQ(total, 70u);
}

TEST_F(XtcStreamingTest, ShortAndNegativeReadsNeverReachTheConsumer) {
  for (const bool negative : {false, true}) {
    storage_test::files["book.xtc"] = makeXtc({{13, 35}});
    xtc::XtcParser parser;
    ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
    storage_test::shortReadAt = FIRST_BITMAP + 32;
    storage_test::negativeRead = negative;
    unsigned bands = 0;
    EXPECT_EQ(
        parser.loadPageStreaming(0, [&](const uint8_t*, size_t, size_t) { ++bands; }, 32), xtc::XtcError::READ_ERROR);
    EXPECT_EQ(parser.getLastError(), xtc::XtcError::READ_ERROR);
    EXPECT_EQ(bands, 1u);
    EXPECT_EQ(storage_test::openHandles, 0u);
  }
}

TEST_F(XtcStreamingTest, TruncatedBitmapFailsBeforeAnyBandIsDrawn) {
  auto bytes = makeXtc({{13, 35}});
  bytes.pop_back();
  storage_test::files["book.xtc"] = bytes;
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
  unsigned bands = 0;
  EXPECT_EQ(
      parser.loadPageStreaming(0, [&](const uint8_t*, size_t, size_t) { ++bands; }, 32), xtc::XtcError::READ_ERROR);
  EXPECT_EQ(bands, 0u);
}

TEST_F(XtcStreamingTest, ZeroChunkAndMissingCallbackFailWithoutAllocatingOrHanging) {
  storage_test::files["book.xtc"] = makeXtc({{13, 35}});
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
  EXPECT_EQ(parser.loadPageStreaming(0, [](const uint8_t*, size_t, size_t) {}, 0), xtc::XtcError::CORRUPTED_HEADER);
  EXPECT_EQ(parser.loadPageStreaming(0, {}, 32), xtc::XtcError::CORRUPTED_HEADER);
  EXPECT_EQ(parser.loadPageStreaming(1, [](const uint8_t*, size_t, size_t) {}, 32), xtc::XtcError::PAGE_OUT_OF_RANGE);
}

TEST_F(XtcStreamingTest, ChunkAllocationFailureIsReportedAndClosesTheFile) {
  storage_test::files["book.xtc"] = makeXtc({{480, 800}});
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
  xtc::XtcError result;
  unsigned bands = 0;
  {
    allocation_test::ArrayFaultScope fault(1);
    result = parser.loadPageStreaming(0, [&](const uint8_t*, size_t, size_t) { ++bands; }, 960);
  }
  EXPECT_EQ(result, xtc::XtcError::MEMORY_ERROR);
  EXPECT_EQ(parser.getLastError(), result);
  EXPECT_EQ(allocation_test::attempts, 1u);
  EXPECT_EQ(bands, 0u);
  EXPECT_EQ(storage_test::openHandles, 0u);
}

TEST_F(XtcStreamingTest, InvalidDimensionsAndCompressedPagesAreRejected) {
  for (unsigned variant = 0; variant < 3; ++variant) {
    auto bytes = makeXtc({{13, 35}});
    if (variant == 0) binary_fixture::put(bytes, FIRST_PAGE + 4, 12, 2);
    if (variant == 1) {
      binary_fixture::put(bytes, TABLE_OFFSET + 12, 0, 2);
      binary_fixture::put(bytes, FIRST_PAGE + 4, 0, 2);
    }
    if (variant == 2) bytes[FIRST_PAGE + 9] = 1;
    storage_test::files["book.xtc"] = bytes;
    xtc::XtcParser parser;
    ASSERT_EQ(parser.open("book.xtc"), xtc::XtcError::OK);
    EXPECT_EQ(parser.loadPageStreaming(0, [](const uint8_t*, size_t, size_t) {}, 32), xtc::XtcError::CORRUPTED_HEADER);
    uint8_t output[70];
    EXPECT_EQ(parser.loadPage(0, output, sizeof(output)), 0u);
    EXPECT_EQ(storage_test::openHandles, 0u);
  }
}

TEST_F(XtcStreamingTest, GrayscaleRetainsBothPlanesAndRejectsUnsafeColumnHeights) {
  storage_test::files["good.xtch"] = makeXtc({{8, 16}}, 2);
  storage_test::files["odd.xtch"] = makeXtc({{8, 15}}, 2);
  xtc::XtcParser parser;
  ASSERT_EQ(parser.open("good.xtch"), xtc::XtcError::OK);
  uint8_t bytes[32]{};
  EXPECT_EQ(parser.loadPage(0, bytes, sizeof(bytes)), sizeof(bytes));
  for (const auto byte : bytes) EXPECT_EQ(byte, 0xA6);
  ASSERT_EQ(parser.open("odd.xtch"), xtc::XtcError::OK);
  EXPECT_EQ(parser.loadPage(0, bytes, sizeof(bytes)), 0u);
  EXPECT_EQ(parser.getLastError(), xtc::XtcError::CORRUPTED_HEADER);
}

TEST(XtcGrayscaleAllocationTest, FirstSuccessDoesNotReleaseCaches) {
  unsigned releases = 0;
  std::unique_ptr<uint8_t[]> bytes;
  {
    allocation_test::ArrayFaultScope fault(0);
    bytes = xtc::allocateGrayscalePage(96000, [&] { ++releases; });
  }
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(allocation_test::attempts, 1u);
  EXPECT_EQ(allocation_test::largest, 96000u);
  EXPECT_EQ(releases, 0u);
}

TEST(XtcGrayscaleAllocationTest, FirstFailureReleasesBeforeTheSingleRetry) {
  unsigned releases = 0, attemptsAtRelease = 0;
  std::unique_ptr<uint8_t[]> bytes;
  {
    allocation_test::ArrayFaultScope fault(1);
    bytes = xtc::allocateGrayscalePage(96000, [&] {
      attemptsAtRelease = allocation_test::attempts;
      ++releases;
    });
  }
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(allocation_test::attempts, 2u);
  EXPECT_EQ(attemptsAtRelease, 1u);
  EXPECT_EQ(releases, 1u);
}

TEST(XtcGrayscaleAllocationTest, SecondFailureReturnsNullWithoutAThirdAllocation) {
  unsigned releases = 0;
  std::unique_ptr<uint8_t[]> bytes;
  {
    allocation_test::ArrayFaultScope fault(2);
    bytes = xtc::allocateGrayscalePage(96000, [&] { ++releases; });
  }
  EXPECT_EQ(bytes, nullptr);
  EXPECT_EQ(allocation_test::attempts, 2u);
  EXPECT_EQ(releases, 1u);
}
