#include <FirmwareBoardTag.h>
#include <FirmwareFlasher.h>
#include <HttpDownloader.h>
#include <KimchiRelease.h>
#include <OtaUpdater.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ota_test {
std::string json, lastUrl;
std::vector<uint8_t> image;
size_t chunkSize = 7;
bool networkOk = true;
uint16_t chip = 5;
}  // namespace ota_test

// Network and silicon writes are mocked; versioning, streaming JSON, update
// state, chip/board checks and the board-tag scanner are production code.
bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& callback, const std::string&,
                              const std::string&) {
  ota_test::lastUrl = url;
  if (!ota_test::networkOk) return false;
  const bool release = url == kimchi_release::LATEST_URL;
  const auto* data = release ? reinterpret_cast<const uint8_t*>(ota_test::json.data()) : ota_test::image.data();
  const size_t size = release ? ota_test::json.size() : ota_test::image.size();
  for (size_t offset = 0; offset < size; offset += ota_test::chunkSize) {
    if (!callback(data + offset, std::min(ota_test::chunkSize, size - offset))) return false;
  }
  return true;
}

uint16_t firmware_flash::runningPartitionChipId() { return ota_test::chip; }

namespace {
void replaceAll(std::string& text, const std::string& before, const std::string& after) {
  size_t offset = 0;
  while ((offset = text.find(before, offset)) != std::string::npos) {
    text.replace(offset, before.size(), after);
    offset += after.size();
  }
}

std::string expectedAsset() {
  return std::string(TEST_BOARD_NAME) == "x4" ? "firmware.bin" : "firmware-" + std::string(TEST_BOARD_NAME) + ".bin";
}

class KimchiOtaTest : public testing::Test {
 protected:
  void SetUp() override {
    ota_test::resetFlash();
    ota_test::networkOk = true;
    ota_test::powerSave = WIFI_PS_MIN_MODEM;
    ota_test::chunkSize = 7;
    ota_test::chip = 5;
    std::ifstream fixture(OTA_FIXTURE_PATH);
    ASSERT_TRUE(fixture.good());
    ota_test::json.assign(std::istreambuf_iterator<char>(fixture), {});
    setImageBoard(TEST_BOARD_NAME);
  }
  static void setImageBoard(const std::string& board) {
    ota_test::image.assign(256, 0);
    ota_test::image[0] = 0xE9;
    ota_test::image[12] = 5;
    const std::string tag = "CROSSPOINT-BOARD-V1:" + board + ";";
    std::copy(tag.begin(), tag.end(), ota_test::image.begin() + 29);
  }
};
}  // namespace

TEST(KimchiVersionTest, ComparesUpstreamTupleThenKimchiRevisionAndNormalizesV) {
  for (const char* tag : {"1.6.0-kimchi.2", "v1.6.0-kimchi.12", "1.6.1-kimchi.1", "1.7.0-kimchi.1", "2.0.0-kimchi.1"})
    EXPECT_TRUE(kimchi_release::isNewer(tag, CROSSPOINT_RELEASE_VERSION)) << tag;
  for (const char* tag : {"1.6.0-kimchi.1", "v1.6.0-kimchi.1", "1.5.99-kimchi.99", "0.99.99-kimchi.99"})
    EXPECT_FALSE(kimchi_release::isNewer(tag, CROSSPOINT_RELEASE_VERSION)) << tag;
  EXPECT_FALSE(kimchi_release::isNewer("1.6.0-kimchi.9", "v1.6.0-kimchi.10"));
  EXPECT_TRUE(kimchi_release::isNewer("v1.6.0-kimchi.11", "v1.6.0-kimchi.10"));
}

TEST(KimchiVersionTest, RejectsMalformedAndForeignTagsWithoutUninitializedNumbers) {
  for (const char* tag :
       {"", "1.6.0", "v1.6", "vv1.6.0-kimchi.2", "1.6.0-kimchi.0", "1.6.0-kimchi.-1", "1.6.0-kimchi.02",
        "01.6.0-kimchi.2", "1.6.0-kimchi.2x", "1.6.0-kimchi.2-rc", "1.6.0-kimchi.2+build", " 1.6.0-kimchi.2",
        "1.6.0-kimchi.2 ", "4294967296.0.0-kimchi.1", "1.6.0-kimchi.4294967296", "sd-fonts-m1-b1"}) {
    kimchi_release::Version parsed;
    EXPECT_FALSE(kimchi_release::parseVersion(tag, parsed)) << tag;
    EXPECT_FALSE(kimchi_release::isNewer(tag, CROSSPOINT_RELEASE_VERSION));
    EXPECT_FALSE(kimchi_release::isNewer("2.0.0-kimchi.1", tag));
  }
}

TEST(KimchiVersionTest, KeepsTheOriginalBoardAssetNames) {
  char output[48];
  for (const char* board : {"x4", "sticky", "x4pro", "papermono", "x4c", "m5paper"}) {
    ASSERT_TRUE(kimchi_release::assetName(board, output, sizeof(output)));
    EXPECT_EQ(output, std::string(board) == "x4" ? "firmware.bin" : "firmware-" + std::string(board) + ".bin");
  }
  EXPECT_FALSE(kimchi_release::assetName("../x4", output, sizeof(output)));
  EXPECT_FALSE(kimchi_release::assetName("", output, sizeof(output)));
  EXPECT_FALSE(kimchi_release::assetName("sticky", output, 2));
}

TEST_F(KimchiOtaTest, UsesKimchiLatestAndTheRealCompiledBoardTag) {
  EXPECT_EQ(std::string(board_tag::boardName(), board_tag::boardNameLen()), TEST_BOARD_NAME);
  for (size_t chunk : {1u, 7u, 31u, 1024u}) {
    ota_test::chunkSize = chunk;
    OtaUpdater updater;
    ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
    EXPECT_EQ(ota_test::lastUrl, kimchi_release::LATEST_URL);
    EXPECT_EQ(updater.getLatestVersion(), "v1.6.0-kimchi.2");
    EXPECT_EQ(updater.getOtaSize(), 256u);
    ASSERT_TRUE(updater.isUpdateNewer());
    EXPECT_EQ(updater.installUpdate(), OtaUpdater::OK);
    EXPECT_EQ(ota_test::lastUrl, std::string(kimchi_release::DOWNLOAD_PREFIX) + "v1.6.0-kimchi.2/" + expectedAsset());
    EXPECT_EQ(updater.getProcessedSize(), 256u);
    EXPECT_EQ(ota_test::declaredSize, 256u);
  }
  EXPECT_EQ(ota_test::boots, 4u);
  EXPECT_EQ(ota_test::ends, 4u);  // ESP's original image verification remains mandatory.
  EXPECT_EQ(ota_test::powerSave, WIFI_PS_MIN_MODEM);
}

TEST_F(KimchiOtaTest, EqualOlderDraftAndPrereleaseAreNotInstallable) {
  const auto original = ota_test::json;
  for (const char* tag : {"1.6.0-kimchi.1", "v1.6.0-kimchi.1", "v1.5.9-kimchi.9"}) {
    ota_test::json = original;
    replaceAll(ota_test::json, "v1.6.0-kimchi.2", tag);
    OtaUpdater updater;
    EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::NO_UPDATE);
    EXPECT_FALSE(updater.isUpdateNewer());
    EXPECT_EQ(updater.installUpdate(), OtaUpdater::UPDATE_OLDER_ERROR);
  }
  for (const char* field : {"draft", "prerelease"}) {
    ota_test::json = original;
    replaceAll(ota_test::json, std::string("\"") + field + "\": false", std::string("\"") + field + "\": true");
    OtaUpdater updater;
    EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::NO_UPDATE);
  }
  EXPECT_EQ(ota_test::begins, 0u);
}

TEST_F(KimchiOtaTest, MissingBoardAssetDoesNotFallBackToAnotherBoard) {
  replaceAll(ota_test::json, expectedAsset(), expectedAsset() + ".bak");
  OtaUpdater updater;
  EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::NO_UPDATE);
  EXPECT_FALSE(updater.isUpdateNewer());
}

TEST_F(KimchiOtaTest, TruncatedReleaseJsonAndInvalidTagsAreRejected) {
  const auto original = ota_test::json;
  for (const char* tag : {"not-a-version", "1.6.0-kimchi.2oops", "sd-fonts-m1-b1"}) {
    ota_test::json = original;
    replaceAll(ota_test::json, "v1.6.0-kimchi.2", tag);
    OtaUpdater updater;
    EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::JSON_PARSE_ERROR);
  }
  // All matching assets have arrived, but the top-level object is incomplete.
  ota_test::json = original.substr(0, original.find_last_of('}'));
  OtaUpdater updater;
  EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::JSON_PARSE_ERROR);
}

TEST_F(KimchiOtaTest, InvalidAssetSizesAndForeignDownloadUrlsCannotBeUsed) {
  const auto original = ota_test::json;
  for (const char* value : {"0", "-1", "2.56e2", "4294967296", "23"}) {
    ota_test::json = original;
    replaceAll(ota_test::json, "\"size\": 256", "\"size\": " + std::string(value));
    OtaUpdater updater;
    EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::JSON_PARSE_ERROR) << value;
  }
  ota_test::json = original;
  replaceAll(ota_test::json, "https://github.com/songhyun-k/", "https://github.com/another-owner/");
  OtaUpdater updater;
  EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::JSON_PARSE_ERROR);
}

TEST_F(KimchiOtaTest, AFailedRecheckClearsStaleDownloadState) {
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  ota_test::networkOk = false;
  EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::HTTP_ERROR);
  EXPECT_FALSE(updater.isUpdateNewer());
  EXPECT_EQ(updater.getOtaSize(), 0u);
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::UPDATE_OLDER_ERROR);
}

TEST_F(KimchiOtaTest, ChecksTheActualInactiveSlotBeforeStartingAnyWrite) {
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  ota_test::partition.size = 255;
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::INTERNAL_UPDATE_ERROR);
  ota_test::havePartition = false;
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::INTERNAL_UPDATE_ERROR);
  EXPECT_EQ(ota_test::begins, 0u);
  EXPECT_TRUE(ota_test::written.empty());
}

TEST_F(KimchiOtaTest, FailedAndTruncatedOrOversizedDownloadsNeverActivateASlot) {
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  ota_test::networkOk = false;
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::HTTP_ERROR);
  ota_test::networkOk = true;
  ota_test::image.resize(250);
  EXPECT_NE(updater.installUpdate(), OtaUpdater::OK);
  ota_test::image.resize(257);
  EXPECT_NE(updater.installUpdate(), OtaUpdater::OK);
  EXPECT_EQ(ota_test::boots, 0u);
  EXPECT_EQ(ota_test::aborts, 3u);
  EXPECT_EQ(ota_test::ends, 0u);
}

TEST_F(KimchiOtaTest, ExistingChipAndStreamingBoardChecksStillRejectWrongImages) {
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  ota_test::image[12] = 9;
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::WRONG_DEVICE_ERROR);
  setImageBoard(std::string(TEST_BOARD_NAME) == "sticky" ? "x4pro" : "sticky");
  ota_test::chunkSize = 1;  // split every chip/header/board-tag boundary
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::WRONG_DEVICE_ERROR);
  EXPECT_EQ(ota_test::boots, 0u);
  EXPECT_EQ(ota_test::aborts, 2u);
}

TEST_F(KimchiOtaTest, PreservesImageVerificationAndAbortOnWriteFailure) {
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  ota_test::writeResult = ESP_FAIL;
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::INTERNAL_UPDATE_ERROR);
  EXPECT_EQ(ota_test::aborts, 1u);
  ota_test::writeResult = ESP_OK;
  ota_test::endResult = ESP_FAIL;  // silicon/IDF rejects image checksum/signature
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::INTERNAL_UPDATE_ERROR);
  EXPECT_EQ(ota_test::ends, 1u);
  EXPECT_EQ(ota_test::boots, 0u);
  EXPECT_EQ(ota_test::powerSave, WIFI_PS_MIN_MODEM);
}

TEST_F(KimchiOtaTest, InvalidMagicIsRejectedBeforeEndOrBootSelection) {
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  ota_test::image[0] = 0;
  EXPECT_NE(updater.installUpdate(), OtaUpdater::OK);
  EXPECT_EQ(ota_test::ends, 0u);
  EXPECT_EQ(ota_test::boots, 0u);
}
