#pragma once
#include <Arduino.h>

// ── Display (GC9A01 — I80 8-bit parallel, via LCD_CAM peripheral) ──────────
// Interface: I80, NOT SPI. User_Setup.h updated to match.
// Data bus D0–D7 must be consecutive GPIOs starting at TFT_D0.
constexpr int TFT_D0_PIN   = 10;  // I80 D0
constexpr int TFT_D1_PIN   = 11;  // I80 D1
constexpr int TFT_D2_PIN   = 12;  // I80 D2
constexpr int TFT_D3_PIN   = 13;  // I80 D3
constexpr int TFT_D4_PIN   = 14;  // I80 D4
constexpr int TFT_D5_PIN   = 15;  // I80 D5
constexpr int TFT_D6_PIN   = 16;  // I80 D6
constexpr int TFT_D7_PIN   = 17;  // I80 D7
constexpr int TFT_WR_PIN   = 3;   // I80 WRB (write strobe, active LOW)
constexpr int TFT_RS_PIN   = 18;  // I80 RS  (0 = command, 1 = data)
constexpr int TFT_CS_PIN   = 2;   // I80 CS  (chip select, active LOW)
constexpr int TFT_RST_PIN  = 21;  // Display reset (active LOW)
constexpr int TFT_BL_PIN   = 42;  // Backlight — NPN Q2, IO42 HIGH = on

// ── Fingerprint sensor (HLK-ZW101 via UART2, routed through J6) ───────────
constexpr int FP_TOUCH_PIN  = 4;   // J6 Pin 9  — ZW101 TOUCH_OUT
constexpr int FP_CTRL_PIN   = 5;   // J6 Pin 10 — VCC power switch (IO5 / IIS_BCLK, connected to U14)
constexpr int FP_UART_TX    = 6;   // J6 Pin 11 — ESP32 TX → ZW101 RX
constexpr int FP_UART_RX    = 7;   // J6 Pin 12 — ZW101 TX → ESP32 RX
constexpr int FP_VTOUCH_PIN = 39;  // J6 Pin 6  — ZW101 V_TOUCH supply (temp: GPIO-driven HIGH, ~10µA)

// ── PCF85063 RTC — I²C (SIIC bus, shared with TCA6408) ───────────────────
constexpr int RTC_SDA_PIN  = 8;   // SIIC_SDA_IO8
constexpr int RTC_SCL_PIN  = 9;   // SIIC_SCL_IO9

// ── CST816S capacitive touch ─────────────────────────────────────────────
constexpr int TOUCH_RST_PIN   = 0;   // CST816 RST — GPIO0 (safe to drive after boot)

// ── Battery monitoring ───────────────────────────────────────────────────
constexpr int BAT_ADC_PIN     = 1;   // GPIO1 / ADC1_CH0 — battery via 100k:100k divider (÷2)

// ── Haptics ──────────────────────────────────────────────────────────────
constexpr int HAPTIC_PIN      = 38;  // vibration motor — 2N2222 low-side switch (base via ~1k)

// ── TCA6408 I/O expander — buttons + IMU interrupts ──────────────────────
// TCA6408_INT_PIN fires (active LOW) whenever any port pin changes state.
constexpr int TCA6408_INT_PIN = 45;   // ESP32 GPIO — direct, active LOW

// Port-bit assignments (TCA6408A P0–P7)
constexpr int TOUCH_INT_BIT  = 0;    // TCA6408 P0 — CST816 Touch_INT (active LOW)
constexpr int IMU_INT1_BIT   = 1;    // TCA6408 P1 — QMI8658 INT1: any-motion / WoM
constexpr int IMU_INT2_BIT   = 2;    // TCA6408 P2 — QMI8658 INT2: tap detection
constexpr int BTN1_BIT       = 3;    // TCA6408 P3 — SW_UP
constexpr int BTN_PWR_BIT    = 4;    // TCA6408 P4 — SW_Power
constexpr int BTN2_BIT       = 5;    // TCA6408 P5 — SW_DOWN
constexpr int CHARGE_DET_BIT = 6;    // TCA6408 P6 — charge detect: LOW = USB/VIN present (charging)

// ── Peripheral initialisation ─────────────────────────────────────────────
void hardware_init();

// Configure deep-sleep wakeup sources: GPIO4 (FP_TOUCH_PIN) ANY_HIGH only.
void hardware_configure_wakeup();

// True only if TOUCH_OUT stays HIGH for the whole window — i.e. something is
// actually driving the line. Used to reject spurious EXT1 wakeups (see
// boot_determine_mode).
bool hardware_fp_touch_held(uint32_t window_ms);
