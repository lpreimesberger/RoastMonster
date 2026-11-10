#pragma once
/**
 * @file pins_rlcd42.h
 * @brief Verified Hardware Pinout for the Waveshare ESP32-S3-RLCD-4.2
 *        (ESP32-S3-WROOM-1-N16R8, 4.2" 400x300 ST7305 Reflective LCD)
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 */

#include <stdint.h>

namespace rlcd42 {

// --- 1. ST7305 4.2" 400x300 Reflective LCD (4-Wire Hardware SPI) ---
static constexpr int PIN_RLCD_SCK  = 11;
static constexpr int PIN_RLCD_MOSI = 12;
static constexpr int PIN_RLCD_MISO = -1;  // Write-only SPI display
static constexpr int PIN_RLCD_DC   = 5;
static constexpr int PIN_RLCD_CS   = 40;
static constexpr int PIN_RLCD_RST  = 41;
static constexpr int PIN_RLCD_TE   = -1;

static constexpr uint16_t RLCD_WIDTH  = 400;
static constexpr uint16_t RLCD_HEIGHT = 300;
static constexpr size_t   RLCD_BUF_BYTES = (RLCD_WIDTH * RLCD_HEIGHT) / 8; // 15,000 bytes

// --- 2. Native USB OTG Interface (Aillio Bullet R1/R2 Host or Serial Debug) ---
// Routed to both the USB-C port and the rear 2x8 header (Row 1, Pins 3 & 4)
static constexpr int PIN_USB_DM = 19; // USB D-
static constexpr int PIN_USB_DP = 20; // USB D+

// --- 3. Shared Internal I2C Bus (SHTC3, PCF85063 RTC, ES8311 DAC, ES7210 ADC) ---
static constexpr int PIN_I2C_SDA = 13;
static constexpr int PIN_I2C_SCL = 14;

static constexpr uint8_t I2C_ADDR_SHTC3    = 0x70; // Ambient Temp & Humidity
static constexpr uint8_t I2C_ADDR_PCF85063 = 0x51; // Real-Time Clock
static constexpr uint8_t I2C_ADDR_ES8311   = 0x18; // Speaker Audio Codec DAC
static constexpr uint8_t I2C_ADDR_ES7210   = 0x40; // Dual-Mic ADC

// --- 4. I2S Audio Bus & Speaker Amplifier (ES8311) ---
static constexpr int PIN_I2S_MCLK  = 16;
static constexpr int PIN_I2S_BCLK  = 9;
static constexpr int PIN_I2S_WS    = 45;
static constexpr int PIN_I2S_DOUT  = 8;  // ESP32 -> ES8311 Speaker
static constexpr int PIN_I2S_DIN   = 10; // ES7210 Mic -> ESP32
static constexpr int PIN_PA_ENABLE = 46; // HIGH enables speaker amplifier

// --- 5. MicroSD Card Slot (1-Bit SD_MMC Mode) ---
// Must call SD_MMC.setPins(21, 38, 39) BEFORE SD_MMC.begin("/sdcard", true)
static constexpr int PIN_SDMMC_CLK = 21;
static constexpr int PIN_SDMMC_CMD = 38;
static constexpr int PIN_SDMMC_D0  = 39;

// --- 6. Onboard Buttons & 18650 Battery Monitor ---
static constexpr int PIN_BTN_BOOT = 0;  // Active LOW (also ROM bootloader strap)
static constexpr int PIN_BTN_KEY  = 18; // Active LOW side user button
static constexpr int PIN_BAT_ADC  = 4;  // 3.0x voltage divider on 18650 rail
static constexpr float BAT_DIVIDER_RATIO = 3.0f;

} // namespace rlcd42
