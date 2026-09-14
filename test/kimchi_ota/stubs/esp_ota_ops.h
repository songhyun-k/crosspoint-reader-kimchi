#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

using esp_err_t = int;
using esp_ota_handle_t = uint32_t;
inline constexpr int ESP_OK = 0;
inline constexpr int ESP_FAIL = -1;
struct esp_partition_t {
  uint32_t size = 6553600;
};

namespace ota_test {
inline esp_partition_t partition;
inline bool havePartition = true;
inline int beginResult = ESP_OK, writeResult = ESP_OK, endResult = ESP_OK, bootResult = ESP_OK;
inline unsigned begins = 0, ends = 0, aborts = 0, boots = 0;
inline size_t declaredSize = 0;
inline std::vector<uint8_t> written;
inline void resetFlash() {
  partition = {};
  havePartition = true;
  beginResult = writeResult = endResult = bootResult = ESP_OK;
  begins = ends = aborts = boots = 0;
  declaredSize = 0;
  written.clear();
}
}  // namespace ota_test

inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
  return ota_test::havePartition ? &ota_test::partition : nullptr;
}
inline esp_err_t esp_ota_begin(const esp_partition_t*, size_t size, esp_ota_handle_t* handle) {
  ++ota_test::begins;
  ota_test::declaredSize = size;
  *handle = 1;
  return ota_test::beginResult;
}
inline esp_err_t esp_ota_write(esp_ota_handle_t, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  ota_test::written.insert(ota_test::written.end(), bytes, bytes + size);
  return ota_test::writeResult;
}
inline esp_err_t esp_ota_end(esp_ota_handle_t) {
  ++ota_test::ends;
  return ota_test::endResult;
}
inline esp_err_t esp_ota_abort(esp_ota_handle_t) {
  ++ota_test::aborts;
  return ESP_OK;
}
inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) {
  ++ota_test::boots;
  return ota_test::bootResult;
}
inline const char* esp_err_to_name(esp_err_t) { return "mock OTA error"; }
