#pragma once

inline constexpr int WIFI_PS_NONE = 0, WIFI_PS_MIN_MODEM = 1;
namespace ota_test {
inline int powerSave = WIFI_PS_MIN_MODEM;
}
inline int esp_wifi_set_ps(int mode) {
  ota_test::powerSave = mode;
  return 0;
}
