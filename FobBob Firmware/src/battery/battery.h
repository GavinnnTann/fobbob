#pragma once
#include <stdint.h>
#include <stdbool.h>

// LIR2450 (rechargeable Li-ion coin cell) monitoring.
//   Voltage: GPIO1 / ADC1_CH0 via a 100k:100k divider (÷2).
//   Charge detect: TCA6408 P6 — LOW = USB/VIN present (charging), HIGH = on battery.

struct BatteryStatus {
    uint8_t  percent;     // 0..100, from a non-linear Li-ion discharge curve
    uint16_t millivolts;  // estimated pack voltage (post-divider × 2)
    bool     charging;    // true while external power is connected
};

// Configure the ADC pin. Call once in setup() after buttons_init().
void battery_init();

// Sample the ADC (averaged) + read the charge-detect line.
BatteryStatus battery_read();

// Instantaneous pack voltage in mV — two ADC samples, no EMA, no I2C.
// battery_read()'s smoothing deliberately erases momentary load sag; this does
// not, so it is the one to use when the question is whether the cell collapses
// under a burst (see the fingerprint rail monitor in main.cpp).
uint16_t battery_raw_mv();
