# BulletSpy S3 — ESP32-S3 + 4.2" RLCD Firmware

Standalone C++17 firmware for the **Waveshare ESP32-S3-RLCD-4.2** (`ESP32-S3-WROOM-1-N16R8`, `400×300` Sitronix `ST7305` Paper-Like Reflective LCD, `SHTC3` Ambient Sensor, `ES8311` Audio Codec, `18650` UPS Battery, and `MicroSD` slot).

Drives the local **400×300 Paper-Like Display at 4Hz** while simultaneously hosting the full **BulletSpy Dark-Mode Web Dashboard** (`http://bulletspy.local`) and `/ws` WebSocket telemetry stream over Wi-Fi.

---

## Verified Hardware Pinout (`include/pins_rlcd42.h`)

| Subsystem | Controller / Bus | ESP32-S3 GPIO Mapping |
| :--- | :--- | :--- |
| **4.2" RLCD (`400×300`)** | Sitronix `ST7305` (SPI) | `SCK=11`, `MOSI=12`, `DC=5`, `CS=40`, `RST=41` |
| **Aillio Bullet USB Host** | Native USB OTG | `USB_D-=19`, `USB_D+=20` (Type-C & Rear 2×8 Header Pins 3–4) |
| **Ambient Temp / RH** | `SHTC3` (`I2C 0x70`) | `SDA=13`, `SCL=14` |
| **Real-Time Clock** | `PCF85063` (`I2C 0x51`) | `SDA=13`, `SCL=14` |
| **Speaker & Chimes** | `ES8311` (`I2C 0x18` + `I2S`) | `MCLK=16`, `BCLK=9`, `WS=45`, `DOUT=8`, `PA_EN=46` |
| **MicroSD Card Slot** | `SD_MMC` (1-Bit Mode) | `CLK=21`, `CMD=38`, `D0=39` |
| **18650 Battery Monitor** | ADC1 (3.0× Divider) | `GPIO 4` |
| **Hardware Buttons** | Active-LOW | `BOOT=0`, `KEY=18` |

---

## Day-1 Quick Start (When Your Board Arrives)

### 1. First-Time Flash over USB-C
Plug the board into your computer via USB-C and build/upload with **PlatformIO**:

```bash
cd esp32
pio run -t upload
```
*(Tip: If the board is already running in USB Host mode and you want to re-flash over USB instead of Wi-Fi OTA, hold the **`BOOT`** button while clicking Reset/Power, or hold the side **`KEY`** button at power-on to keep USB Serial Debug mode active.)*

---

### 2. Setting Up Wi-Fi (Two Zero-Cable Options)

#### Option A: Phone / Laptop Captive Portal (No SD Card Required)
1. On first boot with no saved Wi-Fi credentials, the RLCD screen displays the **Wi-Fi Setup Banner** and broadcasts the **`BulletSpy-Setup`** hotspot.
2. Join **`BulletSpy-Setup`** on your phone or laptop.
3. A setup page automatically opens (or browse to **`http://192.168.4.1`**).
4. Enter your Wi-Fi SSID & password, choose your hostname (`bulletspy`), set your default Target Drop Temp, and tap **Save & Connect**.
5. Settings are saved to internal **NVS flash** and the header bar on the RLCD screen will show your LAN IP address. Open **`http://bulletspy.local`** in any browser!
   *(Even if you are roasting somewhere without Wi-Fi, you can tap the **side `KEY` button** to dismiss the setup banner on the RLCD screen, or open `http://192.168.4.1/dashboard` directly against the ESP32's hotspot.)*

#### Option B: MicroSD `/config.json` Auto-Import
1. Copy [`sdcard_config.example.json`](sdcard_config.example.json) to the root of a FAT32 MicroSD card as **`config.json`**:
   ```json
   {
     "wifi_ssid": "MyRoasteryWiFi",
     "wifi_password": "secretpassword",
     "hostname": "bulletspy",
     "allow_write": true,
     "target_drop_c": 210.0
   }
   ```
2. Insert the MicroSD card into the side slot and power on. The ESP32 imports `config.json` into internal NVS flash and also automatically logs every active roast batch (`cycle == ROASTING`) to **`/roasts/latest_roast.csv`** on the SD card!

---

## Re-Syncing the Embedded Web Dashboard

Whenever you edit the HTML/CSS/JS inside [`../bulletspy.py`](../bulletspy.py), regenerate [`include/dashboard_html.h`](include/dashboard_html.h) (`~11.8 KB` gzipped `PROGMEM`) with:

```bash
../venv/bin/python sync_dashboard.py
```

---

## Running the Desktop C++17 Unit Tests (No Hardware Required)

```bash
g++ -std=c++17 -O2 -Wall -Wextra test/test_native.cpp -o /tmp/test_bulletspy_esp32 && /tmp/test_bulletspy_esp32
```

---

## License, Copyright & Trademarks

**Copyright &copy; 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.**

Licensed under the **[PolyForm Shield License 1.0.0](../LICENSE)** (`SPDX-License-Identifier: PolyForm-Shield-1.0.0`).

- **Required Notice:** `Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.`
- **Licensor Line of Business:** Coffee Roasting Telemetry, Roaster Control Hardware, Embedded Firmware, Roast Analysis Software, and Specialty Coffee Products by Beanzilla Coffee Worldwide LLC.
- **Trademarks:** **Beanzilla&trade;**, **Beanzilla Coffee Worldwide&trade;**, **BulletSpy&trade;**, and the Beanzilla Kaiju logo ([`include/beanzilla_logo.h`](include/beanzilla_logo.h)) are trademarks of **Beanzilla Coffee Worldwide LLC**.
