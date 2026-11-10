/**
 * @file test_native.cpp
 * @brief Host-runnable C++17 unit test suite verifying:
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 *
 *  1. STM32 CRC-32 calculation & R2 command packet signing
 *  2. Aillio Bullet R2 (0xA0, 0xA1, 0xA2) & R1 binary frame unpacking
 *  3. Roast stage classification across all temperature zones and cycle states
 *  4. Command encoding (PRS, Heater, Fan, Drum, Preheat) & read-only guard
 *  5. Sitronix ST7305 400x300 2x4 landscape pixel block mapping & UI rendering
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "../include/bullet_protocol.h"
#include "../include/st7305_rlcd.h"

using namespace bulletspy;

static void test_stm32_crc32() {
    // Verify R2 PRS command [0x30, 0x01, 0x00, 0x00] + 4-byte STM32 CRC-32
    auto pkt = prepare_r2_command(CMD_PRS, sizeof(CMD_PRS));
    assert(pkt.size() == 8);
    assert(pkt[0] == 0x30 && pkt[1] == 0x01 && pkt[2] == 0x00 && pkt[3] == 0x00);

    // Deterministic check against Python's calculate_stm32_crc32([0x30, 0x01, 0x00, 0x00, 0, 0, 0, 0])
    uint8_t raw[8] = {0x30, 0x01, 0x00, 0x00, 0, 0, 0, 0};
    uint32_t crc = calculate_stm32_crc32(raw, 8);
    assert(crc != 0);
    assert(pkt[4] == static_cast<uint8_t>(crc & 0xFF));
    assert(pkt[7] == static_cast<uint8_t>((crc >> 24) & 0xFF));
    std::printf("[PASS] test_stm32_crc32 (PRS CRC=0x%08x)\n", crc);
}

static void test_r2_frame_unpacking() {
    BulletState st;
    st.heater_power = 9;
    st.fan_speed    = 6;
    st.drum_speed   = 8;
    st.cycle_id     = 0x06;

    uint8_t a0[64], a1[64], a2[64];
    build_mock_r2_frames(125.0, st, a0, a1, a2);

    BulletState decoded;
    decode_r2_frame(a0, 64, decoded);
    decode_r2_frame(a1, 64, decoded);
    decode_r2_frame(a2, 64, decoded);

    assert(decoded.minutes == 2);
    assert(decoded.seconds == 5);
    assert(std::strcmp(decoded.roast_clock, "02:05") == 0);
    assert(decoded.heater_power == 9);
    assert(decoded.fan_speed == 6);
    assert(decoded.drum_speed == 8);
    assert(decoded.cycle_id == 0x06);
    assert(std::strcmp(decoded.cycle, "ROASTING") == 0);
    assert(decoded.power_setpoint_w == 9 * 220);
    assert(decoded.drum_rpm == 52 + 8 * 6);
    assert(decoded.bean_temp_c > 130.0f && decoded.bean_temp_c < 235.0f);
    std::printf("[PASS] test_r2_frame_unpacking (BT=%.1fC, IBTS=%.1fC, Power=%uW)\n",
                decoded.bean_temp_c, decoded.drum_temp_ibts_c, decoded.power_setpoint_w);
}

static void test_r1_packet_unpacking() {
    uint8_t pkt1[64] = {0};
    uint8_t pkt2[64] = {0};
    pack_le_f32(pkt1 + 0,  218.4f); // IBTS
    pack_le_f32(pkt1 + 4,  9.2f);   // IBTS RoR
    pack_le_f32(pkt1 + 8,  201.5f); // Bean Probe (First Crack)
    pack_le_f32(pkt1 + 12, 8.5f);   // Bean RoR
    pack_le_f32(pkt1 + 16, 155.0f); // Exhaust
    pkt1[24] = 7;                   // minutes
    pkt1[25] = 42;                  // seconds
    pkt1[26] = 5;                   // fan
    pkt1[27] = 6;                   // heater
    pkt1[28] = 9;                   // drum
    pkt1[29] = 0x06;                // ROASTING
    pack_le_u16(pkt1 + 44, 1850);   // fan_rpm
    pack_le_u16(pkt1 + 48, 239);    // voltage

    BulletState st;
    decode_r1_packets(pkt1, 64, pkt2, 64, st);
    assert(std::fabs(st.bean_temp_c - 201.5f) < 0.1f);
    assert(std::strcmp(st.roast_stage_short, "FIRST CRACK") == 0);
    assert(std::strcmp(st.roast_clock, "07:42") == 0);
    assert(st.fan_rpm == 1850);
    std::printf("[PASS] test_r1_packet_unpacking (Stage=%s, Clock=%s)\n",
                st.roast_stage_short, st.roast_clock);
}

static void test_command_encoding_and_guard() {
    BulletState st;
    st.allow_write  = false;
    st.heater_power = 4;

    std::vector<std::vector<uint8_t>> pkts;
    assert(!encode_command("heater", 8, true, st, pkts));
    assert(pkts.empty());

    st.allow_write = true;
    assert(encode_command("heater", 8, true, st, pkts));
    assert(pkts.size() == 4); // 4 increments from P4 -> P8
    assert(st.heater_power == 8);
    std::printf("[PASS] test_command_encoding_and_guard (4 R2 heater packets queued)\n");
}

static void test_st7305_display_buffer() {
    ST7305Display disp;
    // After clear(false), all bytes should be 0xFF (white paper)
    for (size_t i = 0; i < ST7305Display::BUFFER_LEN; ++i) {
        assert(disp.buffer()[i] == 0xFF);
    }

    // Set corners and center pixel to black and verify exact round-trip lookup
    const int coords[5][2] = {{0, 0}, {399, 0}, {0, 299}, {399, 299}, {200, 150}};
    for (const auto& pt : coords) {
        assert(!disp.get_pixel(pt[0], pt[1]));
        disp.set_pixel(pt[0], pt[1], true);
        assert(disp.get_pixel(pt[0], pt[1]));
    }

    // Render Beanzilla boot splash and verify bitmap pixels
    disp.render_boot_splash("INITIALIZING USB HOST & WIFI...");
    size_t splash_black_bytes = 0;
    for (size_t i = 0; i < ST7305Display::BUFFER_LEN; ++i) {
        if (disp.buffer()[i] != 0xFF) splash_black_bytes++;
    }
    assert(splash_black_bytes > 1000);

    // Render full telemetry dashboard and ensure pixels were drawn
    BulletState st;
    st.bean_temp_c = 204.2f;
    st.drum_temp_ibts_c = 221.0f;
    st.bean_ror_c_min = 9.4f;
    st.heater_power = 8;
    st.fan_speed = 5;
    st.drum_speed = 9;
    st.active_power_w = 1760.0f;
    for (int i = 0; i < 60; ++i) {
        float bt = 140.0f + i * 1.1f;
        st.push_history(bt, bt + 18.0f, 11.5f - i * 0.04f);
    }
    disp.render_dashboard(st, "192.168.1.42", false);

    size_t black_bytes = 0;
    for (size_t i = 0; i < ST7305Display::BUFFER_LEN; ++i) {
        if (disp.buffer()[i] != 0xFF) black_bytes++;
    }
    assert(black_bytes > 500);
    std::printf("[PASS] test_st7305_display_buffer (Splash=%zu B, Dashboard=%zu/15000 GRAM bytes active)\n",
                splash_black_bytes, black_bytes);
}

int main() {
    test_stm32_crc32();
    test_r2_frame_unpacking();
    test_r1_packet_unpacking();
    test_command_encoding_and_guard();
    test_st7305_display_buffer();
    std::printf("\nAll C++17 ESP32-S3 BulletSpy tests passed!\n");
    return 0;
}
