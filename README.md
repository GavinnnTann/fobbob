# <img src="Pictures/FobBob Icon_Only.png" width="40" align="center" alt="FobBob Icon"> FobBob 🔐

### A dedicated hardware authenticator that keeps your 2FA codes off your phone.

![Platform](https://img.shields.io/badge/platform-ESP32--S3-E7352C?logo=espressif&logoColor=white)
![Framework](https://img.shields.io/badge/framework-Arduino-00979D?logo=arduino&logoColor=white)
![Webapp](https://img.shields.io/badge/webapp-React%20%2B%20Vite-61DAFB?logo=react&logoColor=white)
![License](https://img.shields.io/badge/license-MIT-green)

<img src="Pictures/FobBob_Product Image.jpg" width="500" alt="FobBob device">

---

## About

Most people store their 2FA codes on the same phone they use for everything else. If that phone is lost, compromised, or out of battery — so is your access.

**FobBob** is a small, self-contained hardware device that generates TOTP (Time-based One-Time Password) codes independently. Touch the fingerprint sensor, glance at the round display, done. No phone, no app, no internet connection required after setup.

It is provisioned once via a browser-based web app over Bluetooth, then works completely standalone.

---

## Features

- **Fingerprint-gated access** — display only lights up after your fingerprint is verified, keeping codes private even if the device is left on a desk
- **Live TOTP codes** — shows 30-second rotating codes on a crisp 240×240 round display, with a tileview UI to scroll between accounts
- **Google Authenticator compatible** — import existing accounts by scanning a Google Authenticator export QR; export back at any time
- **Browser-based setup** — no software to install; provision from Chrome or Edge using Web Bluetooth
- **Account management** — add, remove, rename, and reorder accounts at any time by re-entering provisioning mode
- **Power efficient** — sleeps in deep sleep between uses; a fingerprint touch is all it takes to wake it
- **Offline time sync** — syncs time over Bluetooth on wake; no Wi-Fi or internet needed
- **Hardware RTC** — PCF85063 battery-backed clock keeps time across full power loss
- **Configurable timeout** — display sleep timer adjustable from the web app (5 s – 5 min)

---

## Prerequisites

Before you get started, make sure you have the following:

**To build the firmware:**
- [VS Code](https://code.visualstudio.com/) with the [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- A USB cable to flash the ESP32

**To run the provisioning web app:**
- [Node.js](https://nodejs.org/) v18 or later
- Google Chrome or Microsoft Edge *(Web Bluetooth is not supported in Safari or Firefox)*

**Hardware:**
- Waveshare ESP32-S3-Zero (or compatible ESP32-S3 board)
- GC9A01 1.28" round display
- HLK-ZW101 capacitive fingerprint sensor
- 3× momentary push buttons (via TCA6408A I²C expander)
- PCF85063 RTC module
- LiPo battery + TP4056 charging module

---

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/GavinnnTann/fobbob.git
cd fobbob
```

### 2. Flash the firmware

Open the `PocketAuth/` folder in VS Code. PlatformIO will automatically install all required libraries on first build.

```bash
# Build and upload to the device
pio run -e lolin_s3_mini -t upload

# Open serial monitor to verify boot
pio device monitor
```

### 3. Set up the provisioning web app

```bash
cd "PocketAuth Webapp"
npm install
npm run dev
```

Open the URL printed in the terminal (e.g., `http://localhost:5173`) in **Chrome or Edge**.

---

## Usage

### First-time setup

1. Power on the device — it will enter **BLE provisioning mode** automatically on first boot
2. Open the web app in Chrome or Edge
3. Click **Connect Device** and select your `FobBob-XXXX` from the Bluetooth picker
4. Enter the 6-digit PIN shown on the device display
5. Give your device a name, import your accounts, and click **Provision Device**
6. The device restarts and is ready to use

### Importing accounts from Google Authenticator

In Google Authenticator on your phone:

> **⋮ menu → Transfer accounts → Export accounts**

Scan the QR code with the FobBob web app camera. All your accounts will be imported automatically.

### Exporting accounts back to Google Authenticator

On the Accounts step in the web app, click **Export** in the account list header. Select the accounts you want, then scan the displayed QR code with Google Authenticator:

> **⋮ menu → Transfer accounts → Import accounts**

### Day-to-day use

Simply touch the fingerprint sensor to wake the device. Your TOTP codes will appear on the display. Use the buttons to scroll between accounts. The device goes back to sleep automatically after the configured timeout (default 30 seconds).

### Re-provisioning (adding or removing accounts)

Hold **both buttons for 5 seconds** to re-enter provisioning mode. Your existing accounts are preserved — the web app will load them so you can edit and send back the updated list.

### Time sync (without re-provisioning)

Click **Sync time only** on the web app connect screen. It pushes the current browser time to the device over Bluetooth in a few seconds, without touching your accounts or settings.

---

## Troubleshooting / FAQ

**The web app can't find my device.**

Web Bluetooth only works in Chrome or Edge on desktop. Make sure Bluetooth is enabled on your computer and the device is powered on and in provisioning mode. If you've just flashed the firmware, try a full power cycle.

---

**I entered the correct PIN but the app says it's wrong.**

The PIN is generated fresh each time provisioning mode starts. If the device restarted or timed out (5-minute window), a new PIN was generated. Restart the device to begin a new session.

---

**My TOTP codes are wrong / out of sync.**

The device needs an accurate time source. Use the **Sync time only** button on the web app connect screen — it pushes the current browser time to the device over Bluetooth in about 5 seconds without going through the full provisioning flow.

---

## Contributing

Contributions are welcome! Here's how to get involved:

1. **Report a bug** — open an issue and describe what happened, what you expected, and your hardware setup
2. **Request a feature** — open an issue with the `enhancement` label
3. **Submit a fix or feature** — fork the repo, create a branch (`git checkout -b feature/your-idea`), commit your changes, and open a pull request against `main`

Please keep pull requests focused — one feature or fix per PR makes review much easier.

---

## License

This project is licensed under the **MIT License** — you are free to use, modify, and distribute it. See the `LICENSE` file for details.
