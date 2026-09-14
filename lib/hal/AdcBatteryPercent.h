#pragma once

#include <cstdint>

namespace battery_percent {

// The X4 ADC tops out near 91% on the observed full-charge curve. Correct only
// its upper range. Board selection and the original EMA remain in the HAL.
inline uint16_t correctX4AdcSample(const uint16_t raw) {
  constexpr uint16_t knee = 80;
  constexpr uint16_t observedFull = 91;
  if (raw <= knee) return raw;
  return raw >= observedFull ? 100
                             : knee + ((raw - knee) * (100 - knee) + (observedFull - knee) / 2) / (observedFull - knee);
}

}  // namespace battery_percent
