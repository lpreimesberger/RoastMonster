#pragma once
/**
 * @file st7305_rlcd.h
 * @brief Hardware driver and 400x300 1-bit Paper-Like Dashboard Renderer for the
 *        Waveshare ESP32-S3-RLCD-4.2 (Sitronix ST7305 Reflective LCD).
 *
 * Uses the exact ST7305 2x4 landscape pixel block layout (15,000-byte GRAM buffer)
 * and Waveshare ST7305 charge-pump/gate initialization sequence.
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "beanzilla_logo.h"
#include "bullet_protocol.h"
#include "pins_rlcd42.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <SPI.h>
#endif

namespace bulletspy {

// Compact 5x7 ASCII bitmap font (0x20 Space .. 0x5F '_')
static constexpr uint8_t FONT5X7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
};

class ST7305Display {
public:
    static constexpr uint16_t WIDTH      = rlcd42::RLCD_WIDTH;       // 400
    static constexpr uint16_t HEIGHT     = rlcd42::RLCD_HEIGHT;      // 300
    static constexpr size_t   BUFFER_LEN = rlcd42::RLCD_BUF_BYTES;   // 15,000

    ST7305Display() {
        clear(false);
    }

    /**
     * @brief Maps (x, y) on the 400x300 landscape panel into the ST7305's 2x4
     *        pixel block byte index and bit mask.
     */
    static inline void map_pixel_landscape(int x, int y, uint16_t& out_byte_idx, uint8_t& out_bit_mask) {
        static constexpr uint16_t H4 = HEIGHT >> 2; // 75 vertical blocks
        const uint16_t inv_y   = static_cast<uint16_t>(HEIGHT - 1 - y);
        const uint16_t block_y = inv_y >> 2;
        const uint8_t  local_y = inv_y & 3;
        const uint16_t byte_x  = static_cast<uint16_t>(x) >> 1;
        const uint8_t  local_x = static_cast<uint8_t>(x) & 1;

        out_byte_idx = static_cast<uint16_t>(byte_x * H4 + block_y);
        const uint8_t bit = static_cast<uint8_t>(7 - ((local_y << 1) | local_x));
        out_bit_mask = static_cast<uint8_t>(1u << bit);
    }

    void clear(bool black = false) {
        // In ST7305 monochrome mode: 0x00 = Black (pixel ON), 0xFF = White/Paper (pixel OFF)
        std::memset(buffer_, black ? 0x00 : 0xFF, BUFFER_LEN);
    }

    inline void set_pixel(int x, int y, bool black = true) {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
        uint16_t idx = 0;
        uint8_t mask = 0;
        map_pixel_landscape(x, y, idx, mask);
        if (black) {
            buffer_[idx] &= static_cast<uint8_t>(~mask);
        } else {
            buffer_[idx] |= mask;
        }
    }

    inline bool get_pixel(int x, int y) const {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return false;
        uint16_t idx = 0;
        uint8_t mask = 0;
        map_pixel_landscape(x, y, idx, mask);
        return (buffer_[idx] & mask) == 0;
    }

    void draw_hline(int x, int y, int w, bool black = true) {
        for (int i = 0; i < w; ++i) {
            set_pixel(x + i, y, black);
        }
    }

    void draw_dashed_hline(int x, int y, int w, int dash = 4, bool black = true) {
        for (int i = 0; i < w; ++i) {
            if (((i / dash) % 2) == 0) {
                set_pixel(x + i, y, black);
            }
        }
    }

    void draw_vline(int x, int y, int h, bool black = true) {
        for (int i = 0; i < h; ++i) {
            set_pixel(x, y + i, black);
        }
    }

    void draw_rect(int x, int y, int w, int h, bool black = true) {
        draw_hline(x, y, w, black);
        draw_hline(x, y + h - 1, w, black);
        draw_vline(x, y, h, black);
        draw_vline(x + w - 1, y, h, black);
    }

    void fill_rect(int x, int y, int w, int h, bool black = true) {
        for (int row = 0; row < h; ++row) {
            draw_hline(x, y + row, w, black);
        }
    }

    void draw_line(int x0, int y0, int x1, int y1, bool black = true, int thickness = 1) {
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while (true) {
            for (int ty = 0; ty < thickness; ++ty) {
                set_pixel(x0, y0 + ty, black);
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void draw_char(int x, int y, char c, int scale = 1, bool black = true) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (c < 0x20 || c > 0x5F) {
            c = '?';
        }
        const uint8_t* glyph = FONT5X7[c - 0x20];
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; ++row) {
                if ((bits >> row) & 0x01) {
                    if (scale <= 1) {
                        set_pixel(x + col, y + row, black);
                    } else {
                        fill_rect(x + col * scale, y + row * scale, scale, scale, black);
                    }
                }
            }
        }
    }

    void draw_text(int x, int y, const char* str, int scale = 1, bool black = true) {
        if (!str) return;
        int cur_x = x;
        const int advance = 6 * scale;
        while (*str) {
            draw_char(cur_x, y, *str, scale, black);
            cur_x += advance;
            ++str;
        }
    }

    /**
     * @brief Draws a packed 1-bit row-major PROGMEM bitmap (MSB first, 1 = foreground).
     */
    void draw_bitmap_1bit(int x, int y, int w, int h, const uint8_t* bmp_progmem, bool fg_black = true) {
        if (!bmp_progmem) return;
        const int bytes_per_row = (w + 7) / 8;
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                const uint8_t b = pgm_read_byte(bmp_progmem + row * bytes_per_row + (col >> 3));
                if ((b >> (7 - (col & 7))) & 0x01) {
                    set_pixel(x + col, y + row, fg_black);
                }
            }
        }
    }

    /**
     * @brief Renders the 400x300 Beanzilla Coffee Worldwide Boot Splash Screen.
     */
    void render_boot_splash(const char* status_msg = "INITIALIZING USB HOST & WIFI...") {
        clear(false); // Paper white background

        // Outer double border frame
        draw_rect(4, 4, WIDTH - 8, HEIGHT - 8, true);
        draw_rect(7, 7, WIDTH - 14, HEIGHT - 14, true);

        // Top inverted banner
        fill_rect(7, 7, WIDTH - 14, 24, true);
        draw_text(44, 12, "BEANZILLA COFFEE WORLDWIDE", 2, false);

        // Centered 240x156 Beanzilla Kaiju Logo
        const int logo_x = (WIDTH - BEANZILLA_BOOT_BITMAP_W) / 2;
        const int logo_y = 38;
        draw_bitmap_1bit(logo_x, logo_y, BEANZILLA_BOOT_BITMAP_W, BEANZILLA_BOOT_BITMAP_H,
                         BEANZILLA_BOOT_BITMAP, true);

        // Subtitle & Firmware Badge
        draw_hline(24, 202, WIDTH - 48, true);
        draw_text(14, 212, "BULLETSPY S3 // ROAST TELEMETRY", 2, true);
        draw_text(98, 234, "AILLIO BULLET R1 / R2 USB OTG HOST", 1, true);

        // Bottom boot status pill
        fill_rect(24, 254, WIDTH - 48, 26, true);
        if (status_msg && status_msg[0]) {
            int len = static_cast<int>(std::strlen(status_msg));
            int tx = (WIDTH - len * 6) / 2;
            if (tx < 30) tx = 30;
            draw_text(tx, 263, status_msg, 1, false);
        }
    }

    /**
     * @brief Renders the full 400x300 Paper-Like Roast Telemetry UI into the
     *        1-bit framebuffer, including Beanzilla Coffee Worldwide branding.
     */
    void render_dashboard(const BulletState& st, const char* ip_or_status, bool ap_setup_mode = false) {
        clear(false); // Paper white background

        // --- 1. Top Inverted Header Bar (400 x 24) ---
        fill_rect(0, 0, WIDTH, 24, true);
        draw_text(6, 5, "BEANZILLA", 2, false);

        char badge[32];
        std::snprintf(badge, sizeof(badge), "[%s]", st.roast_stage_short);
        draw_text(122, 8, badge, 1, false);

        if (ip_or_status && ip_or_status[0]) {
            int len = static_cast<int>(std::strlen(ip_or_status));
            int tx = WIDTH - (len * 6) - 6;
            if (tx < 245) tx = 245;
            draw_text(tx, 8, ip_or_status, 1, false);
        }

        // --- 2. Sub-header Status Strip (y = 25..42) ---
        char sub[80];
        std::snprintf(sub, sizeof(sub),
                      "MODE:%s  CYCLE:%s  CRACKS:%u  AMB:%.1FC/%.0F%%  BAT:%u%%",
                      st.mode, st.cycle, st.crack_count,
                      st.room_temp_c, st.room_humidity_pct, st.battery_pct);
        draw_text(6, 29, sub, 1, true);
        draw_hline(0, 41, WIDTH, true);

        if (ap_setup_mode) {
            // Captive Portal Onboarding Banner
            draw_rect(16, 54, WIDTH - 32, 200, true);
            fill_rect(16, 54, WIDTH - 32, 24, true);
            draw_text(28, 60, "WIFI SETUP MODE ACTIVE (CAPTIVE PORTAL)", 1, false);
            draw_text(28, 92,  "1. JOIN WIFI NETWORK:", 1, true);
            draw_text(40, 108, "BULLETSPY-SETUP", 2, true);
            draw_text(28, 136, "2. OPEN BROWSER TO:", 1, true);
            draw_text(40, 152, "HTTP://192.168.4.1", 2, true);
            draw_text(28, 182, "3. OR PLACE /SDCARD/CONFIG.JSON ON MICROSD", 1, true);
            draw_text(28, 202, "   PRESS SIDE [KEY] BUTTON TO DISMISS", 1, true);
            draw_bitmap_1bit(WIDTH - 80, 96, BEANZILLA_MINI_BITMAP_W, BEANZILLA_MINI_BITMAP_H,
                             BEANZILLA_MINI_BITMAP, true);
            return;
        }

        // --- 3. Left Telemetry Column (x = 0..158, y = 42..252) ---
        draw_vline(158, 42, 210, true);

        // Bean Probe Temperature (BT)
        draw_text(6, 47, "BEAN PROBE (BT)", 1, true);
        char bt_str[24];
        std::snprintf(bt_str, sizeof(bt_str), "%5.1FC", st.bean_temp_c);
        draw_text(6, 60, bt_str, 3, true);

        // Drum IBTS Temperature (DT)
        draw_hline(0, 92, 158, true);
        draw_text(6, 97, "DRUM IBTS (DT)", 1, true);
        char dt_str[24];
        std::snprintf(dt_str, sizeof(dt_str), "%5.1FC", st.drum_temp_ibts_c);
        draw_text(6, 110, dt_str, 3, true);

        // Bean RoR & IBTS RoR
        draw_hline(0, 142, 158, true);
        draw_text(6, 147, "BEAN ROR (C/MIN)", 1, true);
        char ror_str[24];
        std::snprintf(ror_str, sizeof(ror_str), "%+5.1F", st.bean_ror_c_min);
        draw_text(6, 160, ror_str, 3, true);

        // Roast Clock + Drop Target
        draw_hline(0, 192, 158, true);
        draw_text(6, 198, "CLOCK", 1, true);
        draw_text(82, 198, "TARGET DROP", 1, true);
        draw_text(6, 212, st.roast_clock, 2, true);
        char drop_str[16];
        std::snprintf(drop_str, sizeof(drop_str), "%.0FC", st.target_drop_c);
        draw_text(82, 212, drop_str, 2, true);

        // Progress Bar (y = 236..248)
        draw_rect(6, 236, 146, 10, true);
        int fill_w = static_cast<int>((st.roast_progress_pct / 100.0f) * 142.0f);
        if (fill_w < 0) fill_w = 0;
        if (fill_w > 142) fill_w = 142;
        if (fill_w > 0) {
            fill_rect(8, 238, fill_w, 6, true);
        }

        // --- 4. Right Live Rolling Roast Curve Chart (x = 160..398, y = 44..250) ---
        const int gx = 166;
        const int gy = 56;
        const int gw = 226;
        const int gh = 186;

        draw_text(166, 46, "CURVE: BT(SOLID) DT(DOT) DROP(--)", 1, true);
        draw_rect(gx, gy, gw, gh, true);

        // Horizontal temperature grid lines (100C, 150C, 200C, 250C across 80C..260C)
        static constexpr float T_MIN = 80.0f;
        static constexpr float T_MAX = 260.0f;
        auto temp_to_y = [&](float temp_c) -> int {
            float norm = (temp_c - T_MIN) / (T_MAX - T_MIN);
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            return gy + gh - 2 - static_cast<int>(norm * (gh - 4));
        };

        for (int grid_t : {120, 160, 200, 240}) {
            int py = temp_to_y(static_cast<float>(grid_t));
            draw_dashed_hline(gx + 1, py, gw - 2, 2, true);
        }

        // Target Drop Temp reference line (longer dashes)
        int drop_y = temp_to_y(st.target_drop_c);
        draw_dashed_hline(gx + 1, drop_y, gw - 2, 6, true);

        // Mini Beanzilla Mascot in bottom-right corner of the chart
        draw_bitmap_1bit(gx + gw - BEANZILLA_MINI_BITMAP_W - 4,
                         gy + gh - BEANZILLA_MINI_BITMAP_H - 3,
                         BEANZILLA_MINI_BITMAP_W, BEANZILLA_MINI_BITMAP_H,
                         BEANZILLA_MINI_BITMAP, true);

        // Plot rolling BT (2px solid) & IBTS (dotted) history
        if (st.hist_count >= 2) {
            const size_t n = st.hist_count;
            const size_t start_idx = (st.hist_head + HISTORY_MAX_POINTS - n) % HISTORY_MAX_POINTS;
            for (size_t i = 1; i < n; ++i) {
                size_t idx0 = (start_idx + i - 1) % HISTORY_MAX_POINTS;
                size_t idx1 = (start_idx + i) % HISTORY_MAX_POINTS;

                int x0 = gx + 2 + static_cast<int>(((i - 1) * (gw - 4)) / (HISTORY_MAX_POINTS - 1));
                int x1 = gx + 2 + static_cast<int>((i * (gw - 4)) / (HISTORY_MAX_POINTS - 1));

                int bt_y0 = temp_to_y(st.hist_bt[idx0]);
                int bt_y1 = temp_to_y(st.hist_bt[idx1]);
                draw_line(x0, bt_y0, x1, bt_y1, true, 2);

                if ((i % 2) == 0) {
                    int dt_y1 = temp_to_y(st.hist_ibts[idx1]);
                    fill_rect(x1, dt_y1, 2, 2, true);
                }
            }
        }

        // --- 5. Bottom Control & Electrical Telemetry Bar (y = 252..299) ---
        draw_hline(0, 252, WIDTH, true);

        // 4 actuator boxes: HEATER, FAN, DRUM, POWER
        const int box_w = 94;
        const int box_y = 256;
        const int box_h = 40;

        char p_str[16], f_str[16], d_str[16], w_str[16];
        std::snprintf(p_str, sizeof(p_str), "P%u", st.heater_power);
        std::snprintf(f_str, sizeof(f_str), "F%u", st.fan_speed);
        std::snprintf(d_str, sizeof(d_str), "D%u", st.drum_speed);
        std::snprintf(w_str, sizeof(w_str), "%.0FW", st.active_power_w);

        const char* labels[4] = {"HEATER", "FAN", "DRUM", "POWER"};
        const char* vals[4]   = {p_str, f_str, d_str, w_str};

        for (int i = 0; i < 4; ++i) {
            int bx = 6 + i * (box_w + 4);
            draw_rect(bx, box_y, box_w, box_h, true);
            draw_text(bx + 6, box_y + 5, labels[i], 1, true);
            draw_text(bx + 6, box_y + 18, vals[i], 2, true);
        }
    }

    const uint8_t* buffer() const { return buffer_; }
    uint8_t* mutable_buffer() { return buffer_; }

#ifdef ARDUINO
    bool begin() {
        pinMode(rlcd42::PIN_RLCD_DC, OUTPUT);
        pinMode(rlcd42::PIN_RLCD_CS, OUTPUT);
        pinMode(rlcd42::PIN_RLCD_RST, OUTPUT);
        digitalWrite(rlcd42::PIN_RLCD_CS, HIGH);
        digitalWrite(rlcd42::PIN_RLCD_DC, HIGH);

        spi_.begin(rlcd42::PIN_RLCD_SCK, rlcd42::PIN_RLCD_MISO, rlcd42::PIN_RLCD_MOSI, rlcd42::PIN_RLCD_CS);
        hardware_reset();
        init_registers();
        display();
        return true;
    }

    void hardware_reset() {
        digitalWrite(rlcd42::PIN_RLCD_RST, HIGH);
        delay(50);
        digitalWrite(rlcd42::PIN_RLCD_RST, LOW);
        delay(20);
        digitalWrite(rlcd42::PIN_RLCD_RST, HIGH);
        delay(50);
    }

    void init_registers() {
        // Exact Sitronix ST7305 initialization for Waveshare 4.2" 400x300 RLCD
        send_cmd(0xD6); send_data(0x17); send_data(0x02); // NVM Load Control
        send_cmd(0xD1); send_data(0x01);                  // Booster Enable
        send_cmd(0xC0); send_data(0x11); send_data(0x04); // Gate Voltage VGH/VGL
        send_cmd(0xC1); send_data(0x69); send_data(0x69); send_data(0x69); send_data(0x69); // VSHP
        send_cmd(0xC2); send_data(0x19); send_data(0x19); send_data(0x19); send_data(0x19); // VSLP
        send_cmd(0xC4); send_data(0x4B); send_data(0x4B); send_data(0x4B); send_data(0x4B); // VSHN
        send_cmd(0xC5); send_data(0x19); send_data(0x19); send_data(0x19); send_data(0x19); // VSLN
        send_cmd(0xD8); send_data(0x80); send_data(0xE9); // OSC Setting
        send_cmd(0xB2); send_data(0x02);                  // Frame Rate Control

        // Gate EQ Control (High Power Mode)
        send_cmd(0xB3);
        const uint8_t eq_hp[10] = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
        for (uint8_t b : eq_hp) send_data(b);

        // Gate EQ Control (Low Power Mode)
        send_cmd(0xB4);
        const uint8_t eq_lp[8] = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
        for (uint8_t b : eq_lp) send_data(b);

        send_cmd(0x62); send_data(0x32); send_data(0x03); send_data(0x1F); // Gate Timing
        send_cmd(0xB7); send_data(0x13); // Source EQ Enable
        send_cmd(0xB0); send_data(0x64); // 300 gate lines (100 * 3)

        send_cmd(0x11); // Sleep Out
        delay(200);

        send_cmd(0xC9); send_data(0x00); // Source Voltage Select
        send_cmd(0x36); send_data(0x48); // MADCTL (MX=1, DO=1)
        send_cmd(0x3A); send_data(0x11); // 1-bit monochrome format
        send_cmd(0xB9); send_data(0x20); // Gamma Mode
        send_cmd(0xB8); send_data(0x29); // Panel Setting (1-dot inversion)
        send_cmd(0x21);                  // Display Inversion On

        send_cmd(0x2A); send_data(0x12); send_data(0x2A); // Column Address 0x12..0x2A
        send_cmd(0x2B); send_data(0x00); send_data(0xC7); // Row Address 0x00..0xC7
        send_cmd(0x35); send_data(0x00);                  // TE Line On
        send_cmd(0xD0); send_data(0xFF);                  // Auto Power Down
        send_cmd(0x38);                                   // High Power Refresh Mode (~51Hz)
        send_cmd(0x29);                                   // Display On
    }

    /**
     * @brief Streams the 15,000-byte GRAM buffer over 10MHz SPI (<15ms refresh).
     */
    void display() {
        send_cmd(0x38);
        send_cmd(0x29);
        send_cmd(0x2A); send_data(0x12); send_data(0x2A);
        send_cmd(0x2B); send_data(0x00); send_data(0xC7);

        spi_.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
        digitalWrite(rlcd42::PIN_RLCD_DC, LOW);
        digitalWrite(rlcd42::PIN_RLCD_CS, LOW);
        spi_.transfer(0x2C); // Memory Write

        digitalWrite(rlcd42::PIN_RLCD_DC, HIGH);
        spi_.writeBytes(buffer_, BUFFER_LEN);
        digitalWrite(rlcd42::PIN_RLCD_CS, HIGH);
        spi_.endTransaction();
    }

private:
    SPIClass spi_{FSPI};

    void send_cmd(uint8_t cmd) {
        spi_.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
        digitalWrite(rlcd42::PIN_RLCD_DC, LOW);
        digitalWrite(rlcd42::PIN_RLCD_CS, LOW);
        spi_.transfer(cmd);
        digitalWrite(rlcd42::PIN_RLCD_CS, HIGH);
        spi_.endTransaction();
    }

    void send_data(uint8_t data) {
        spi_.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
        digitalWrite(rlcd42::PIN_RLCD_DC, HIGH);
        digitalWrite(rlcd42::PIN_RLCD_CS, LOW);
        spi_.transfer(data);
        digitalWrite(rlcd42::PIN_RLCD_CS, HIGH);
        spi_.endTransaction();
    }
#endif

private:
    uint8_t buffer_[BUFFER_LEN];
};

} // namespace bulletspy
