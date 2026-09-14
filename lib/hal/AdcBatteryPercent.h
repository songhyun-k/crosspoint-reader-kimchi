#pragma once

#include <cstdint>

namespace battery_percent {

// The X4 ADC tops out near 91% on the observed full-charge curve. Correct only
// its upper range, BEFORE the existing tenths-of-a-percent EMA. Other ADC/PMIC
// boards retain their original readings; gauge boards never call this path.
inline uint16_t smoothAdcSample(uint16_t raw, const bool isX4Adc, int& cachedTenths) {
  constexpr uint16_t knee = 80;
  constexpr uint16_t observedFull = 91;
  if (isX4Adc && raw > knee) {
    raw = raw >= observedFull
              ? 100
              : knee + ((raw - knee) * (100 - knee) + (observedFull - knee) / 2) / (observedFull - knee);
  }
  cachedTenths = cachedTenths == 0 ? 10 * raw : (cachedTenths * 9 + raw * 10) / 10;
  return static_cast<uint16_t>(cachedTenths / 10);
}

}  // namespace battery_percent
