#pragma once

// No CPU-frequency or electrical power hardware in the native task fixture.
class HalPowerManager {
 public:
  struct Lock {
    Lock() {}
  };
};
