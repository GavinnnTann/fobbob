# FobBob

### A dedicated hardware authenticator that keeps your 2FA codes off your phone.

![Platform](https://img.shields.io/badge/platform-ESP32--S3-E7352C?logo=espressif&logoColor=white)
![Framework](https://img.shields.io/badge/framework-Arduino-00979D?logo=arduino&logoColor=white)
![Webapp](https://img.shields.io/badge/webapp-React%20%2B%20Vite-61DAFB?logo=react&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-green)

---

## About

Most people store their 2FA codes on the same phone they use for everything else. If that phone is lost, compromised, or out of battery — so is your access.

**FobBob** is a small, self-contained hardware device that generates TOTP codes independently. Touch the fingerprint sensor, glance at the round display, done. No phone, no app, no internet connection required after setup.

Provisioned once via a browser-based web app over Bluetooth, then works completely standalone.

---

## Features

- **Fingerprint-gated access** — display only lights up after your fingerprint is verified; codes stay private even if the device is left on a desk
- **Live TOTP codes** — 30-second rotating codes on a crisp 240×240 round display; swipe or use the buttons to move between accounts
- **Capacitive touch UI** — swipe to navigate, tap to hide/reveal a code for privacy, long-press for an on-device settings page
- **Haptic feedback** — a subtle vibration confirms fingerprint unlock, taps, charging, and sleep
- **Battery aware** — on-screen battery percentage and an animated charging indicator when plugged in
- **Auto-rotate** — the display flips to stay upright when you turn the device over (optional)
- **Google Authenticator compatible** — import existing accounts by scanning a Google Authenticator export QR; export back at any time
- **Browser-based setup** — no software to install; provision from Chrome or Edge using Web Bluetooth
- **Account management** — add, remove, rename, and reorder accounts at any time by re-entering provisioning mode
- **Power efficient** — deep sleeps between uses; a fingerprint touch is all it takes to wake it
- **Offline time sync** — syncs time over Bluetooth on wake; no Wi-Fi or internet needed
- **Hardware RTC** — PCF85063 battery-backed clock keeps time across full power loss

---

## Hardware

| Component | Part |
|---|---|
| MCU | Waveshare ESP32-S3-Zero (16MB flash, 16MB PSRAM) |
| Display | 1.28" GC9A01 round LCD — 240×240, I80 parallel |
| Touch | CST816 capacitive touch (on the display) |
| Fingerprint | HLK-ZW101 capacitive sensor |
| IMU | QMI8658C 6-axis (orientation / motion) |
| Buttons | 3× via TCA6408A I²C expander |
| Haptics | Vibration motor (2N2222 driver) |
| RTC | PCF85063 battery-backed |
| Power | LiPo (LIR2450) + charger, with battery-voltage sensing |

---

## Prerequisites

**To build the firmware:**
- [VS Code](https://code.visualstudio.com/) with the [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- A USB cable to flash the ESP32

**To run the provisioning web app:**
- [Node.js](https://nodejs.org/) v18 or later
- Google Chrome or Microsoft Edge *(Web Bluetooth is not supported in Safari or Firefox)*

---

## Installation

### 1. Flash the firmware

Open the `PocketAuth/` folder in VS Code. PlatformIO will automatically install all required libraries on first build.

```bash
pio run -e lolin_s3_mini -t upload
pio device monitor
```

#### (Optional) Flash the WiFi setup portal

For the WiFi-based provisioning fallback (iOS / Safari / Firefox — no Web
Bluetooth), the device serves a small captive-portal site from a LittleFS image.
This is **separate** from the firmware flash and must be uploaded explicitly after
any change to `data/www/`:

```bash
# one-time: vendor jsQR for on-device QR scanning (the AP has no internet)
curl -o data/www/jsqr.min.js https://cdn.jsdelivr.net/npm/jsqr@1.4.0/dist/jsQR.js

# build + flash the LittleFS image (data/ → "spiffs" partition)
pio run -e lolin_s3_mini -t uploadfs
```

Without this step the BLE provisioning path (Chrome/Edge web app) still works; only
the WiFi-AP fallback portal is unavailable (it falls back to a "files not flashed"
notice, and QR scanning degrades to manual secret entry if `jsqr.min.js` is absent).

### 2. Set up the provisioning web app

```bash
cd "PocketAuth Webapp"
npm install
npm run dev
```

Open the URL printed in the terminal in **Chrome or Edge**.

---

## Usage

### First-time setup

1. Power on the device — it enters **BLE provisioning mode** automatically on first boot
2. Open the web app in Chrome or Edge
3. Click **Connect Device** and select your `FobBob-XXXX` from the Bluetooth picker
4. Enter the 6-digit PIN shown on the device display
5. Give your device a name, import your accounts, and click **Provision Device**
6. The device restarts and is ready to use

### Importing accounts from Google Authenticator

In Google Authenticator on your phone:

> **⋮ menu → Transfer accounts → Export accounts**

Scan the QR code with the FobBob web app. All accounts import automatically.

### Exporting accounts back to Google Authenticator

On the Accounts step in the web app, click **Export**. Select the accounts you want, then scan the displayed QR code with Google Authenticator:

> **⋮ menu → Transfer accounts → Import accounts**

### Day-to-day use

Touch the fingerprint sensor to wake the device — a short vibration confirms the unlock and your TOTP codes appear. Navigate accounts by **swiping up/down** or with the side buttons (hold a button to run quickly to the top/bottom). **Tap** the screen to hide/reveal the current code; **long-press** to open the on-device settings page (motion options, screen timeout, and reprovision). Plugging in shows an animated charging indicator. The device sleeps automatically after the idle timeout (configurable, default 30 s) — tap the power button or double-tap the screen to sleep immediately.

### Re-provisioning (adding or removing accounts)

Hold **both side buttons for 5 seconds**, or hold the **Reprovision** row in the on-device settings page, to re-enter provisioning mode. Your existing accounts are preserved — the web app loads them so you can edit and send back the updated list.

### Time sync (without re-provisioning)

Click **Sync time only** on the web app connect screen. It pushes the current browser time to the device over Bluetooth in a few seconds without touching your accounts.

---

## Troubleshooting

**The web app can't find my device.**
Web Bluetooth only works in Chrome or Edge on desktop. Make sure Bluetooth is enabled and the device is powered on and in provisioning mode. Try a full power cycle if you've just flashed the firmware.

**I entered the correct PIN but the app says it's wrong.**
The PIN is generated fresh each time provisioning mode starts. If the device restarted or timed out (5-minute window), a new PIN was generated. Restart the device to get a new session.

**My TOTP codes are wrong / out of sync.**
Use the **Sync time only** button on the web app connect screen to push the current browser time to the device over Bluetooth.

---

## License

MIT — free to use, modify, and distribute.
