#pragma once
/**
 * @file audio_alerts.h
 * @brief Onboard SHTC3 Ambient Sensor Reader, 18650 Battery Monitor (GPIO4),
 *        and ES8311 + I2S Audio Alert Synthesizer for the Waveshare ESP32-S3-RLCD-4.2.
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 */

#include <cmath>
#include <cstdint>

#include "bullet_protocol.h"
#include "pins_rlcd42.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#endif

namespace bulletspy {

class BoardPeripherals {
public:
#ifdef ARDUINO
    void begin() {
        pinMode(rlcd42::PIN_BTN_KEY, INPUT_PULLUP);
        pinMode(rlcd42::PIN_BTN_BOOT, INPUT_PULLUP);
        pinMode(rlcd42::PIN_PA_ENABLE, OUTPUT);
        digitalWrite(rlcd42::PIN_PA_ENABLE, LOW);

        Wire.begin(rlcd42::PIN_I2C_SDA, rlcd42::PIN_I2C_SCL, 400000);
        init_es8311_codec();
        init_i2s_speaker();
    }

    /**
     * @brief Reads the onboard SHTC3 sensor (0x70) and 18650 battery divider (GPIO4).
     */
    void poll_sensors(float& out_temp_c, float& out_rh_pct, float& out_bat_v, uint8_t& out_bat_pct) {
        // 1. Read 18650 battery voltage (GPIO4 via 3.0x voltage divider)
        uint32_t mv = analogReadMilliVolts(rlcd42::PIN_BAT_ADC);
        float vbat = (mv / 1000.0f) * rlcd42::BAT_DIVIDER_RATIO;
        if (vbat < 2.5f || vbat > 4.5f) {
            vbat = 4.15f; // USB powered with no 18650 inserted
        }
        out_bat_v = std::round(vbat * 100.0f) / 100.0f;
        float pct = ((vbat - 3.30f) / (4.18f - 3.30f)) * 100.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        out_bat_pct = static_cast<uint8_t>(pct);

        // 2. Wake & Read SHTC3 (0x70)
        Wire.beginTransmission(rlcd42::I2C_ADDR_SHTC3);
        Wire.write(0x35); Wire.write(0x17); // Wakeup
        if (Wire.endTransmission() != 0) {
            return;
        }
        delayMicroseconds(300);

        Wire.beginTransmission(rlcd42::I2C_ADDR_SHTC3);
        Wire.write(0x78); Wire.write(0x66); // Measure T first (normal mode)
        Wire.endTransmission();
        delay(14);

        if (Wire.requestFrom(rlcd42::I2C_ADDR_SHTC3, static_cast<uint8_t>(6)) == 6) {
            uint16_t raw_t  = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
            Wire.read(); // CRC
            uint16_t raw_rh = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
            Wire.read(); // CRC

            out_temp_c = std::round((-45.0f + 175.0f * (static_cast<float>(raw_t) / 65535.0f)) * 10.0f) / 10.0f;
            out_rh_pct = std::round((100.0f * (static_cast<float>(raw_rh) / 65535.0f)) * 10.0f) / 10.0f;
        }
    }

    /**
     * @brief Evaluates roast state transitions and triggers local ES8311 speaker chimes:
     *        - Preheat Complete (CHARGE READY)
     *        - First Crack (196.0°C)
     *        - Approaching Drop Target (within 2.5°C)
     *        - Target Reached (DROP NOW!)
     */
    void check_roast_alarms(const BulletState& st) {
        if (st.cycle_id == 0x04 && last_cycle_id_ != 0x04) {
            play_tone(880, 140);
            play_tone(1175, 220);
        }
        last_cycle_id_ = st.cycle_id;

        if (st.cycle_id != 0x06) {
            fc_chime_played_   = false;
            near_chime_played_ = false;
            drop_chime_played_ = false;
            return;
        }

        if (!fc_chime_played_ && st.bean_temp_c >= 196.0f) {
            fc_chime_played_ = true;
            play_tone(660, 120);
            play_tone(880, 180);
        }
        if (!near_chime_played_ && st.bean_temp_c >= (st.target_drop_c - 2.5f)) {
            near_chime_played_ = true;
            play_tone(1046, 150);
            play_tone(1046, 150);
        }
        if (!drop_chime_played_ && st.bean_temp_c >= st.target_drop_c) {
            drop_chime_played_ = true;
            play_tone(1318, 180);
            play_tone(1568, 280);
        }
    }

    void play_tone(uint16_t freq_hz, uint16_t duration_ms) {
        if (!i2s_ready_ || freq_hz == 0) return;
        digitalWrite(rlcd42::PIN_PA_ENABLE, HIGH);

        static constexpr uint32_t SAMPLE_RATE = 16000;
        const size_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;
        int16_t stereo_buf[128 * 2];
        size_t written_samples = 0;

        while (written_samples < total_samples) {
            size_t chunk = total_samples - written_samples;
            if (chunk > 128) chunk = 128;
            for (size_t i = 0; i < chunk; ++i) {
                float phase = (2.0f * 3.14159265f * freq_hz * (written_samples + i)) / SAMPLE_RATE;
                int16_t sample = static_cast<int16_t>(std::sin(phase) * 7000.0f);
                stereo_buf[i * 2]     = sample;
                stereo_buf[i * 2 + 1] = sample;
            }
            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, stereo_buf, chunk * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(50));
            written_samples += chunk;
        }
        digitalWrite(rlcd42::PIN_PA_ENABLE, LOW);
    }

private:
    bool i2s_ready_         = false;
    uint8_t last_cycle_id_  = 0xFF;
    bool fc_chime_played_   = false;
    bool near_chime_played_ = false;
    bool drop_chime_played_ = false;

    void write_es8311_reg(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(rlcd42::I2C_ADDR_ES8311);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }

    void init_es8311_codec() {
        write_es8311_reg(0x00, 0x80); // Reset
        delay(10);
        write_es8311_reg(0x00, 0x00);
        write_es8311_reg(0x01, 0x3F); // Enable clocks
        write_es8311_reg(0x02, 0x00);
        write_es8311_reg(0x0D, 0x01); // Power up analog
        write_es8311_reg(0x12, 0x00); // Enable DAC
        write_es8311_reg(0x32, 0xBF); // DAC volume ~75%
        write_es8311_reg(0x37, 0x08);
    }

    void init_i2s_speaker() {
        i2s_config_t cfg = {
            .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
            .sample_rate = 16000,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count = 4,
            .dma_buf_len = 128,
            .use_apll = false,
            .tx_desc_auto_clear = true,
        };
        i2s_pin_config_t pins = {
            .mck_io_num   = rlcd42::PIN_I2S_MCLK,
            .bck_io_num   = rlcd42::PIN_I2S_BCLK,
            .ws_io_num    = rlcd42::PIN_I2S_WS,
            .data_out_num = rlcd42::PIN_I2S_DOUT,
            .data_in_num  = rlcd42::PIN_I2S_DIN,
        };
        if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) == ESP_OK) {
            i2s_set_pin(I2S_NUM_0, &pins);
            i2s_ready_ = true;
        }
    }
#endif
};

} // namespace bulletspy
