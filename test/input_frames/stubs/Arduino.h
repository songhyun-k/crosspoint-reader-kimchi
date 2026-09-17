#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
constexpr int HIGH = 1, LOW = 0, INPUT = 0, OUTPUT = 1, INPUT_PULLUP = 2, INPUT_PULLDOWN = 3, ADC_11db = 3;
unsigned long millis();
int analogRead(int pin);
int analogReadMilliVolts(int pin);
int digitalRead(int pin);
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void analogSetAttenuation(int) {}
inline void delay(unsigned long) {}
struct HardwareSerial {};
inline HardwareSerial Serial, Serial0;
