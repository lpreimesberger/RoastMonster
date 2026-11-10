#pragma once
/**
 * @file bullet_protocol.h
 * @brief Portable C++17 port of BulletSpy's Aillio Bullet R2 & R1 binary protocol
 *        unpacker, STM32 CRC-32 generator, command encoder, roast stage tracker,
 *        and simulated binary frame synthesizer.
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bulletspy {

static constexpr uint16_t AILLIO_VID          = 0x0483;
static constexpr uint16_t PID_BULLET_R2       = 0xA4CD;
static constexpr uint16_t PID_BULLET_R1_IBTS  = 0xA27E;
static constexpr uint16_t PID_BULLET_R1       = 0x5741;

static constexpr uint8_t  AILLIO_INTERFACE    = 0x01;
static constexpr uint8_t  DEFAULT_EP_OUT      = 0x03;
static constexpr uint8_t  DEFAULT_EP_IN       = 0x81;
static constexpr size_t   FRAME_SIZE          = 64;
static constexpr size_t   HISTORY_MAX_POINTS  = 240; // 4 minutes @ 1Hz or 10 min @ 0.4Hz

// Opcodes (shared across R1 & R2; R2 appends 4-byte little-endian STM32 CRC-32)
static constexpr uint8_t CMD_STATUS1[2]     = {0x30, 0x01};
static constexpr uint8_t CMD_STATUS2[2]     = {0x30, 0x03};
static constexpr uint8_t CMD_PRS[4]         = {0x30, 0x01, 0x00, 0x00};
static constexpr uint8_t CMD_HEATER_INCR[4] = {0x34, 0x01, 0xAA, 0xAA};
static constexpr uint8_t CMD_HEATER_DECR[4] = {0x34, 0x02, 0xAA, 0xAA};
static constexpr uint8_t CMD_DRUM_INCR[4]   = {0x32, 0x01, 0xAA, 0xAA};
static constexpr uint8_t CMD_DRUM_DECR[4]   = {0x32, 0x02, 0xAA, 0xAA};
static constexpr uint8_t CMD_FAN_INCR[4]    = {0x31, 0x01, 0xAA, 0xAA};
static constexpr uint8_t CMD_FAN_DECR[4]    = {0x31, 0x02, 0xAA, 0xAA};

struct RoastStageInfo {
    const char* stage;
    const char* stage_short;
    const char* stage_desc;
    const char* stage_color;
    float       progress_pct;
};

struct BulletState {
    double      timestamp_sec      = 0.0;
    const char* mode               = "SIMULATED";
    const char* model              = "Simulated Aillio Bullet R2";
    uint8_t     protocol           = 2;
    bool        allow_write        = false;

    uint8_t     cycle_id           = 0x00;
    const char* cycle              = "OFF";
    uint8_t     minutes            = 0;
    uint8_t     seconds            = 0;
    char        roast_clock[8]     = "00:00";

    // Core temperatures & RoR
    float       drum_temp_ibts_c   = 0.0f;
    float       drum_ror_c_min     = 0.0f;
    float       bean_temp_c        = 0.0f;
    float       bean_ror_c_min     = 0.0f;
    float       exhaust_temp_c     = 0.0f;
    float       inlet_temp_c       = 0.0f;
    float       hot_air_temp_c     = 0.0f;
    float       ibts_ambient_c     = 0.0f;

    // Setpoints & mechanical speeds
    uint8_t     heater_power       = 0;
    uint8_t     fan_speed          = 1;
    uint8_t     drum_speed         = 1;
    uint16_t    fan_rpm            = 0;
    uint16_t    drum_rpm           = 0;
    uint16_t    ibts_fan_rpm       = 0;
    uint16_t    induction_fan_rpm  = 0;

    // Electrical / R2 power metrics
    float       voltage_v          = 0.0f;
    float       current_a          = 0.0f;
    float       active_power_w     = 0.0f;
    uint16_t    power_setpoint_w   = 0;
    float       line_freq_hz       = 0.0f;
    float       igbt_temp1_c       = 0.0f;
    float       igbt_temp2_c       = 0.0f;
    float       energy_used_wh     = 0.0f;

    // Environment & events
    float       diff_pressure_pa   = 0.0f;
    float       pressure_hpa       = 0.0f;
    float       humidity_pct       = 0.0f;
    uint8_t     crack_count        = 0;
    uint8_t     error_counts       = 0;

    // Onboard ESP32-S3-RLCD-4.2 Room SHTC3 & 18650 Battery metrics
    float       room_temp_c        = 24.0f;
    float       room_humidity_pct  = 45.0f;
    float       battery_v          = 4.10f;
    uint8_t     battery_pct        = 95;
    float       target_drop_c      = 210.0f;

    // Roast stage classification
    const char* roast_stage        = "IDLE / STANDBY";
    const char* roast_stage_short  = "STANDBY";
    const char* roast_stage_desc   = "Roaster idle - waiting for preheat";
    const char* roast_stage_color  = "#6b7280";
    float       roast_progress_pct = 0.0f;

    // Raw packet hex inspector (64 bytes -> 128 hex chars + null)
    char        raw_pkt1_hex[129]  = {0};
    char        raw_pkt2_hex[129]  = {0};
    char        raw_a0_hex[129]    = {0};
    char        raw_a1_hex[129]    = {0};
    char        raw_a2_hex[129]    = {0};

    // Rolling history buffer for the 400x300 RLCD curve plot
    float       hist_bt[HISTORY_MAX_POINTS]   = {0};
    float       hist_ibts[HISTORY_MAX_POINTS] = {0};
    float       hist_ror[HISTORY_MAX_POINTS]  = {0};
    size_t      hist_count                    = 0;
    size_t      hist_head                     = 0;

    void push_history(float bt, float ibts, float ror) {
        hist_bt[hist_head]   = bt;
        hist_ibts[hist_head] = ibts;
        hist_ror[hist_head]  = ror;
        hist_head = (hist_head + 1) % HISTORY_MAX_POINTS;
        if (hist_count < HISTORY_MAX_POINTS) {
            hist_count++;
        }
    }
};

// --- Little-endian binary unpack helpers ---
inline float unpack_le_f32(const uint8_t* p) {
    float val = 0.0f;
    std::memcpy(&val, p, sizeof(float));
    return std::round(val * 10.0f) / 10.0f;
}

inline float unpack_le_f32_2dec(const uint8_t* p) {
    float val = 0.0f;
    std::memcpy(&val, p, sizeof(float));
    return std::round(val * 100.0f) / 100.0f;
}

inline uint16_t unpack_le_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline int16_t unpack_le_i16(const uint8_t* p) {
    return static_cast<int16_t>(unpack_le_u16(p));
}

inline void pack_le_f32(uint8_t* p, float val) {
    std::memcpy(p, &val, sizeof(float));
}

inline void pack_le_u16(uint8_t* p, uint16_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

inline void pack_le_u32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

inline void bytes_to_hex(const uint8_t* data, size_t len, char* out_hex) {
    static constexpr char HEX_LUT[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out_hex[i * 2]     = HEX_LUT[(data[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = HEX_LUT[data[i] & 0x0F];
    }
    out_hex[len * 2] = '\0';
}

inline const char* map_cycle_state(uint8_t cycle_id) {
    switch (cycle_id) {
        case 0x00: return "OFF";
        case 0x02: return "PREHEATING";
        case 0x03: return "STABILIZING";
        case 0x04: return "CHARGE";
        case 0x06: return "ROASTING";
        case 0x07: return "COOLDOWN";
        case 0x08: return "COOLING";
        case 0x09: return "SHUTDOWN";
        case 0x0B: return "POWER_ON_RESET";
        default:   return "UNKNOWN";
    }
}

/**
 * @brief Maps bean temperature (°C) and roaster cycle state into a human-readable
 *        roast level / chemical phase label, description, color, and progress %.
 *        Matches bulletspy.py's compute_roast_stage() exactly.
 */
inline RoastStageInfo compute_roast_stage(float bean_temp_c, uint8_t cycle_id = 0x06) {
    float raw_pct = ((bean_temp_c - 90.0f) / (232.0f - 90.0f)) * 100.0f;
    if (raw_pct < 0.0f) raw_pct = 0.0f;
    if (raw_pct > 100.0f) raw_pct = 100.0f;
    float progress_pct = std::round(raw_pct * 10.0f) / 10.0f;

    if (cycle_id == 0x00) {
        return {"IDLE / STANDBY", "STANDBY", "Roaster idle — waiting for preheat", "#6b7280", 0.0f};
    }
    if (cycle_id == 0x02 || cycle_id == 0x03) {
        return {"PREHEATING DRUM", "PREHEAT", "Thermal soak — bringing drum to charge setpoint", "#f59e0b", 0.0f};
    }
    if (cycle_id == 0x04) {
        return {"PREHEAT COMPLETE // CHARGE", "CHARGE READY", "Preheat complete — ready to drop green beans!", "#10b981", 0.0f};
    }
    if (cycle_id == 0x07 || cycle_id == 0x08) {
        return {"COOLING BEANS", "COOLING", "Roast dropped — cooling tray active", "#3b82f6", 100.0f};
    }

    if (bean_temp_c < 150.0f) {
        return {"DRYING PHASE", "DRYING", "Endothermic moisture evaporation (<150°C)", "#10b981", progress_pct};
    }
    if (bean_temp_c < 175.0f) {
        return {"YELLOWING / MAILLARD", "MAILLARD", "Browning reactions & bread/hay aromas (150–175°C)", "#eab308", progress_pct};
    }
    if (bean_temp_c < 196.0f) {
        return {"CARAMELIZATION (PRE-FC)", "PRE-1ST CRACK", "Sucrose caramelization, approaching First Crack (175–196°C)", "#f59e0b", progress_pct};
    }
    if (bean_temp_c < 205.0f) {
        return {"FIRST CRACK (CINNAMON / LIGHT)", "FIRST CRACK", "Exothermic steam pop — Light / New England roast (196–205°C)", "#f97316", progress_pct};
    }
    if (bean_temp_c < 214.0f) {
        return {"CITY ROAST (MEDIUM-LIGHT)", "CITY", "1st Crack clearing — bright, balanced origin character (205–214°C)", "#ea580c", progress_pct};
    }
    if (bean_temp_c < 220.0f) {
        return {"CITY+ / FULL CITY (MEDIUM)", "FULL CITY", "Edge of 2nd Crack — rich body & chocolate/caramel (214–220°C)", "#ef4444", progress_pct};
    }
    if (bean_temp_c < 226.0f) {
        return {"FULL CITY+ / VIENNA (MED-DARK)", "VIENNA", "Rolling 2nd Crack — surface oil droplets (220–226°C)", "#dc2626", progress_pct};
    }
    if (bean_temp_c < 232.0f) {
        return {"FRENCH ROAST (DARK)", "FRENCH", "Heavy roast character, smoky sugars & glossy oils (226–232°C)", "#a855f7", progress_pct};
    }
    return {"ITALIAN / DARK+ (DROP NOW!)", "ITALIAN / DROP!", "High carbonization risk (>232°C) — Drop immediately!", "#ec4899", 100.0f};
}

/**
 * @brief STM32 hardware CRC-32 used by Aillio Bullet R2 command & telemetry frames.
 *        Zeroes the trailing 4 bytes before computing the big-endian word nibble lookup.
 */
inline uint32_t calculate_stm32_crc32(const uint8_t* data, size_t len) {
    static constexpr uint32_t SHORT_LOOKUP[16] = {
        0x00000000u, 0x04C11DB7u, 0x09823B6Eu, 0x0D4326D9u,
        0x130476DCu, 0x17C56B6Bu, 0x1A864DB2u, 0x1E475005u,
        0x2608EDB8u, 0x22C9F00Fu, 0x2F8AD6D6u, 0x2B4BCB61u,
        0x350C9B64u, 0x31CD86D3u, 0x3C8EA00Au, 0x384FBDBDu,
    };

    if (len < 4 || (len % 4) != 0) {
        return 0;
    }

    uint32_t result = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t val = 0;
        // Trailing 4 bytes are zeroed prior to CRC calculation
        if (i + 4 < len) {
            val = (static_cast<uint32_t>(data[i])     << 24) |
                  (static_cast<uint32_t>(data[i + 1]) << 16) |
                  (static_cast<uint32_t>(data[i + 2]) << 8)  |
                  (static_cast<uint32_t>(data[i + 3]));
        }
        uint32_t rax = result ^ val;
        for (int step = 0; step < 4; ++step) {
            uint8_t idx = (rax >> 28) & 0x0Fu;
            rax = (rax << 4) ^ SHORT_LOOKUP[idx];
        }
        result = rax;
    }
    return result;
}

/**
 * @brief Appends the 4-byte little-endian STM32 CRC-32 required by Bullet R2 commands.
 */
inline std::vector<uint8_t> prepare_r2_command(const uint8_t* cmd, size_t cmd_len) {
    std::vector<uint8_t> raw(cmd, cmd + cmd_len);
    raw.push_back(0);
    raw.push_back(0);
    raw.push_back(0);
    raw.push_back(0);
    uint32_t crc = calculate_stm32_crc32(raw.data(), raw.size());
    size_t n = raw.size();
    raw[n - 4] = static_cast<uint8_t>(crc & 0xFF);
    raw[n - 3] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    raw[n - 2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    raw[n - 1] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    return raw;
}

/**
 * @brief Unpacks a 64-byte Aillio Bullet R2 frame (0xA0 Temp, 0xA1 Fan, 0xA2 Power).
 */
inline void decode_r2_frame(const uint8_t* data, size_t len, BulletState& st) {
    if (len < FRAME_SIZE) return;

    const uint8_t frame_type = data[0];
    if (frame_type == 0xA0) {
        st.drum_temp_ibts_c = unpack_le_f32(data + 4);
        st.drum_ror_c_min   = unpack_le_f32(data + 8);
        st.ibts_ambient_c   = unpack_le_f32(data + 12);
        st.bean_temp_c      = unpack_le_f32(data + 16);
        st.bean_ror_c_min   = unpack_le_f32(data + 20);
        st.energy_used_wh   = unpack_le_f32(data + 24);
        st.diff_pressure_pa = unpack_le_f32_2dec(data + 28);
        st.fan_rpm          = unpack_le_u16(data + 32);
        st.inlet_temp_c     = unpack_le_u16(data + 34) / 10.0f;
        st.hot_air_temp_c   = unpack_le_u16(data + 36) / 10.0f;
        st.exhaust_temp_c   = unpack_le_u16(data + 38) / 10.0f;
        st.pressure_hpa     = unpack_le_u16(data + 40) / 10.0f;
        st.humidity_pct     = unpack_le_u16(data + 42) / 10.0f;

        st.minutes          = data[46];
        st.seconds          = data[48];
        std::snprintf(st.roast_clock, sizeof(st.roast_clock), "%02u:%02u", st.minutes, st.seconds);
        st.heater_power     = data[50];
        st.fan_speed        = data[51];
        st.drum_speed       = data[53];
        st.crack_count      = data[56];
        st.cycle_id         = data[59];
        st.cycle            = map_cycle_state(st.cycle_id);

        RoastStageInfo stage = compute_roast_stage(st.bean_temp_c, st.cycle_id);
        st.roast_stage        = stage.stage;
        st.roast_stage_short  = stage.stage_short;
        st.roast_stage_desc   = stage.stage_desc;
        st.roast_stage_color  = stage.stage_color;
        st.roast_progress_pct = stage.progress_pct;

        bytes_to_hex(data, FRAME_SIZE, st.raw_a0_hex);
        bytes_to_hex(data, FRAME_SIZE, st.raw_pkt1_hex);
    } else if (frame_type == 0xA1) {
        st.error_counts      = data[4];
        st.induction_fan_rpm = unpack_le_u16(data + 38);
        st.ibts_fan_rpm      = unpack_le_u16(data + 46);
        st.drum_rpm          = unpack_le_u16(data + 48);
        bytes_to_hex(data, FRAME_SIZE, st.raw_a1_hex);
    } else if (frame_type == 0xA2) {
        st.power_setpoint_w  = unpack_le_u16(data + 4);
        st.line_freq_hz      = unpack_le_u16(data + 6) / 100.0f;
        st.igbt_temp1_c      = unpack_le_u16(data + 12) / 10.0f;
        st.igbt_temp2_c      = unpack_le_u16(data + 14) / 10.0f;
        st.active_power_w    = unpack_le_u16(data + 22) / 10.0f;
        st.voltage_v         = unpack_le_u16(data + 28) / 10.0f;
        st.current_a         = std::round((unpack_le_u16(data + 30) / 1000.0f) * 100.0f) / 100.0f;
        bytes_to_hex(data, FRAME_SIZE, st.raw_a2_hex);
        bytes_to_hex(data, FRAME_SIZE, st.raw_pkt2_hex);
    }
}

/**
 * @brief Unpacks two 64-byte classic Aillio Bullet R1 / IBTS packets.
 */
inline void decode_r1_packets(const uint8_t* pkt1, size_t len1,
                              const uint8_t* pkt2, size_t len2,
                              BulletState& st) {
    if (len1 >= FRAME_SIZE) {
        st.drum_temp_ibts_c = unpack_le_f32(pkt1 + 0);
        st.drum_ror_c_min   = unpack_le_f32(pkt1 + 4);
        st.bean_temp_c      = unpack_le_f32(pkt1 + 8);
        st.bean_ror_c_min   = unpack_le_f32(pkt1 + 12);
        st.exhaust_temp_c   = unpack_le_f32(pkt1 + 16);

        st.minutes          = pkt1[24];
        st.seconds          = pkt1[25];
        std::snprintf(st.roast_clock, sizeof(st.roast_clock), "%02u:%02u", st.minutes, st.seconds);
        st.fan_speed        = pkt1[26];
        st.heater_power     = pkt1[27];
        st.drum_speed       = pkt1[28];
        st.cycle_id         = pkt1[29];
        st.cycle            = map_cycle_state(st.cycle_id);
        st.fan_rpm          = static_cast<uint16_t>(unpack_le_i16(pkt1 + 44));
        st.voltage_v        = static_cast<float>(unpack_le_i16(pkt1 + 48));

        RoastStageInfo stage = compute_roast_stage(st.bean_temp_c, st.cycle_id);
        st.roast_stage        = stage.stage;
        st.roast_stage_short  = stage.stage_short;
        st.roast_stage_desc   = stage.stage_desc;
        st.roast_stage_color  = stage.stage_color;
        st.roast_progress_pct = stage.progress_pct;

        bytes_to_hex(pkt1, FRAME_SIZE, st.raw_pkt1_hex);
    }
    if (len2 >= FRAME_SIZE) {
        bytes_to_hex(pkt2, FRAME_SIZE, st.raw_pkt2_hex);
    }
}

/**
 * @brief Synthesizes realistic 64-byte 0xA0, 0xA1, and 0xA2 binary frames with
 *        valid STM32 CRC-32 checksums when no roaster is plugged into USB.
 */
inline void build_mock_r2_frames(double elapsed_sec, const BulletState& st,
                                 uint8_t out_a0[64], uint8_t out_a1[64], uint8_t out_a2[64]) {
    std::memset(out_a0, 0, FRAME_SIZE);
    std::memset(out_a1, 0, FRAME_SIZE);
    std::memset(out_a2, 0, FRAME_SIZE);

    uint32_t roast_sec = static_cast<uint32_t>(elapsed_sec) % 600;
    uint8_t minutes    = static_cast<uint8_t>(roast_sec / 60);
    uint8_t seconds    = static_cast<uint8_t>(roast_sec % 60);

    float bt            = std::round((182.0f + 42.0f * std::sin(elapsed_sec / 35.0)) * 10.0f) / 10.0f;
    float bt_ror        = std::round(((42.0f / 35.0f) * 60.0f * std::cos(elapsed_sec / 35.0) * 0.2f) * 10.0f) / 10.0f;
    float ibts          = std::round((bt + 21.5f + 3.5f * std::cos(elapsed_sec / 12.0)) * 10.0f) / 10.0f;
    float ibts_ror      = std::round((bt_ror + 1.4f * std::sin(elapsed_sec / 15.0)) * 10.0f) / 10.0f;
    float exhaust       = std::round((bt - 42.0f) * 10.0f) / 10.0f;
    float inlet         = std::round((28.5f + 1.2f * std::sin(elapsed_sec / 40.0)) * 10.0f) / 10.0f;
    float hot_air       = std::round((ibts + 45.0f) * 10.0f) / 10.0f;
    float energy_wh     = std::round(((roast_sec / 600.0f) * 420.0f) * 10.0f) / 10.0f;
    float diff_pressure = std::round((18.5f + 2.1f * std::sin(elapsed_sec / 8.0)) * 100.0f) / 100.0f;

    uint8_t heater   = st.heater_power ? st.heater_power : 7;
    uint8_t fan      = st.fan_speed ? st.fan_speed : 4;
    uint8_t drum     = st.drum_speed ? st.drum_speed : 9;
    uint8_t cycle_id = st.cycle_id ? st.cycle_id : 0x06;

    // Frame 0xA0
    out_a0[0] = 0xA0;
    pack_le_f32(out_a0 + 4,  ibts);
    pack_le_f32(out_a0 + 8,  ibts_ror);
    pack_le_f32(out_a0 + 12, 34.2f);
    pack_le_f32(out_a0 + 16, bt);
    pack_le_f32(out_a0 + 20, bt_ror);
    pack_le_f32(out_a0 + 24, energy_wh);
    pack_le_f32(out_a0 + 28, diff_pressure);
    pack_le_u16(out_a0 + 32, static_cast<uint16_t>(1420 + fan * 110));
    pack_le_u16(out_a0 + 34, static_cast<uint16_t>(inlet * 10.0f));
    pack_le_u16(out_a0 + 36, static_cast<uint16_t>(hot_air * 10.0f));
    pack_le_u16(out_a0 + 38, static_cast<uint16_t>(exhaust * 10.0f));
    pack_le_u16(out_a0 + 40, 10132);
    pack_le_u16(out_a0 + 42, 445);
    pack_le_u16(out_a0 + 44, 265);
    out_a0[46] = minutes;
    out_a0[48] = seconds;
    out_a0[50] = heater;
    out_a0[51] = fan;
    out_a0[53] = drum;
    out_a0[56] = (bt > 196.0f) ? 12 : 0;
    out_a0[59] = cycle_id;
    pack_le_u32(out_a0 + 60, calculate_stm32_crc32(out_a0, FRAME_SIZE));

    // Frame 0xA1
    out_a1[0] = 0xA1;
    out_a1[4] = 0;
    pack_le_u16(out_a1 + 38, 2850);
    pack_le_u16(out_a1 + 46, 4100);
    pack_le_u16(out_a1 + 48, static_cast<uint16_t>(52 + drum * 6));
    pack_le_u32(out_a1 + 60, calculate_stm32_crc32(out_a1, FRAME_SIZE));

    // Frame 0xA2
    out_a2[0] = 0xA2;
    uint16_t watts = static_cast<uint16_t>(heater * 220);
    float volts    = std::round((238.4f + 0.8f * std::sin(elapsed_sec / 5.0)) * 10.0f) / 10.0f;
    float amps     = (volts > 0.0f) ? (std::round((watts / volts) * 100.0f) / 100.0f) : 0.0f;
    pack_le_u16(out_a2 + 4,  watts);
    pack_le_u16(out_a2 + 6,  6000);
    pack_le_u16(out_a2 + 8,  24800);
    pack_le_u16(out_a2 + 12, 462);
    pack_le_u16(out_a2 + 14, 448);
    pack_le_u16(out_a2 + 22, static_cast<uint16_t>(watts * 10));
    pack_le_u16(out_a2 + 28, static_cast<uint16_t>(volts * 10.0f));
    pack_le_u16(out_a2 + 30, static_cast<uint16_t>(amps * 1000.0f));
    pack_le_u32(out_a2 + 60, calculate_stm32_crc32(out_a2, FRAME_SIZE));
}

/**
 * @brief Encodes a control action ("prs", "heater_up", "heater_down", "heater",
 *        "fan_up", "fan_down", "fan", "drum", "preheat") into USB packets and
 *        updates simulated state when running in mock mode.
 */
inline bool encode_command(const std::string& action, int value, bool has_value,
                           BulletState& st, std::vector<std::vector<uint8_t>>& out_packets) {
    if (!st.allow_write) {
        return false;
    }

    auto wrap_pkt = [&](const uint8_t* raw, size_t len) -> std::vector<uint8_t> {
        if (st.protocol == 2) {
            return prepare_r2_command(raw, len);
        }
        return std::vector<uint8_t>(raw, raw + len);
    };

    const bool is_mock = (std::strcmp(st.mode, "SIMULATED") == 0);
    const int max_p    = (st.protocol == 2) ? 14 : 9;

    if (action == "prs") {
        out_packets.push_back(wrap_pkt(CMD_PRS, sizeof(CMD_PRS)));
        if (is_mock) {
            static constexpr uint8_t SEQ[5] = {0x00, 0x02, 0x04, 0x06, 0x08};
            uint8_t nxt = 0x06;
            for (size_t i = 0; i < 5; ++i) {
                if (SEQ[i] == st.cycle_id) {
                    nxt = SEQ[(i + 1) % 5];
                    break;
                }
            }
            st.cycle_id = nxt;
            st.cycle    = map_cycle_state(nxt);
        }
        return true;
    }

    if (action == "heater_up" || action == "heater_down") {
        const uint8_t* op = (action == "heater_up") ? CMD_HEATER_INCR : CMD_HEATER_DECR;
        out_packets.push_back(wrap_pkt(op, 4));
        if (is_mock) {
            int next_p = static_cast<int>(st.heater_power) + ((action == "heater_up") ? 1 : -1);
            if (next_p < 0) next_p = 0;
            if (next_p > max_p) next_p = max_p;
            st.heater_power = static_cast<uint8_t>(next_p);
        }
        return true;
    }

    if (action == "heater" && has_value) {
        int target = value < 0 ? 0 : (value > max_p ? max_p : value);
        int cur    = static_cast<int>(st.heater_power);
        const uint8_t* op = (target > cur) ? CMD_HEATER_INCR : CMD_HEATER_DECR;
        int steps  = std::abs(target - cur);
        for (int i = 0; i < steps; ++i) {
            out_packets.push_back(wrap_pkt(op, 4));
        }
        st.heater_power = static_cast<uint8_t>(target);
        return true;
    }

    if (action == "fan_up" || action == "fan_down") {
        const uint8_t* op = (action == "fan_up") ? CMD_FAN_INCR : CMD_FAN_DECR;
        out_packets.push_back(wrap_pkt(op, 4));
        if (is_mock) {
            int next_f = static_cast<int>(st.fan_speed) + ((action == "fan_up") ? 1 : -1);
            if (next_f < 1) next_f = 1;
            if (next_f > 12) next_f = 12;
            st.fan_speed = static_cast<uint8_t>(next_f);
        }
        return true;
    }

    if (action == "fan" && has_value) {
        int target = value < 1 ? 1 : (value > 12 ? 12 : value);
        int cur    = static_cast<int>(st.fan_speed);
        const uint8_t* op = (target > cur) ? CMD_FAN_INCR : CMD_FAN_DECR;
        int steps  = std::abs(target - cur);
        for (int i = 0; i < steps; ++i) {
            out_packets.push_back(wrap_pkt(op, 4));
        }
        st.fan_speed = static_cast<uint8_t>(target);
        return true;
    }

    if (action == "drum" && has_value) {
        int target = value < 1 ? 1 : (value > 9 ? 9 : value);
        if (st.protocol == 2) {
            int cur = static_cast<int>(st.drum_speed);
            const uint8_t* op = (target > cur) ? CMD_DRUM_INCR : CMD_DRUM_DECR;
            int steps = std::abs(target - cur);
            for (int i = 0; i < steps; ++i) {
                out_packets.push_back(wrap_pkt(op, 4));
            }
        } else {
            uint8_t cmd[4] = {0x32, 0x01, static_cast<uint8_t>(target), 0x00};
            out_packets.push_back(std::vector<uint8_t>(cmd, cmd + 4));
        }
        st.drum_speed = static_cast<uint8_t>(target);
        return true;
    }

    if (action == "preheat" && has_value) {
        int temp = value < 20 ? 20 : (value > 350 ? 350 : value);
        uint8_t cmd[4] = {
            0x35,
            0x00,
            static_cast<uint8_t>((temp >> 8) & 0xFF),
            static_cast<uint8_t>(temp & 0xFF)
        };
        out_packets.push_back(wrap_pkt(cmd, 4));
        return true;
    }

    return false;
}

/**
 * @brief Serializes BulletState into the exact JSON schema expected by the
 *        BulletSpy web dashboard (/ws and /api/state).
 */
inline std::string state_to_json(const BulletState& st) {
    char buf[2048];
    std::snprintf(
        buf, sizeof(buf),
        "{"
        "\"timestamp\":%.3f,"
        "\"mode\":\"%s\","
        "\"model\":\"%s\","
        "\"protocol\":%u,"
        "\"allow_write\":%s,"
        "\"cycle_id\":%u,"
        "\"cycle\":\"%s\","
        "\"roast_clock\":\"%s\","
        "\"minutes\":%u,"
        "\"seconds\":%u,"
        "\"drum_temp_ibts_c\":%.1f,"
        "\"drum_ror_c_min\":%.1f,"
        "\"bean_temp_c\":%.1f,"
        "\"bean_ror_c_min\":%.1f,"
        "\"exhaust_temp_c\":%.1f,"
        "\"inlet_temp_c\":%.1f,"
        "\"hot_air_temp_c\":%.1f,"
        "\"ibts_ambient_c\":%.1f,"
        "\"heater_power\":%u,"
        "\"fan_speed\":%u,"
        "\"drum_speed\":%u,"
        "\"fan_rpm\":%u,"
        "\"drum_rpm\":%u,"
        "\"ibts_fan_rpm\":%u,"
        "\"induction_fan_rpm\":%u,"
        "\"voltage_v\":%.1f,"
        "\"current_a\":%.2f,"
        "\"active_power_w\":%.1f,"
        "\"power_setpoint_w\":%u,"
        "\"line_freq_hz\":%.2f,"
        "\"igbt_temp1_c\":%.1f,"
        "\"igbt_temp2_c\":%.1f,"
        "\"energy_used_wh\":%.1f,"
        "\"diff_pressure_pa\":%.2f,"
        "\"pressure_hpa\":%.1f,"
        "\"humidity_pct\":%.1f,"
        "\"crack_count\":%u,"
        "\"error_counts\":%u,"
        "\"room_temp_c\":%.1f,"
        "\"room_humidity_pct\":%.1f,"
        "\"battery_v\":%.2f,"
        "\"battery_pct\":%u,"
        "\"roast_stage\":\"%s\","
        "\"roast_stage_short\":\"%s\","
        "\"roast_stage_desc\":\"%s\","
        "\"roast_stage_color\":\"%s\","
        "\"roast_progress_pct\":%.1f,"
        "\"raw_pkt1_hex\":\"%s\","
        "\"raw_pkt2_hex\":\"%s\","
        "\"raw_frames\":{\"a0_temp\":\"%s\",\"a1_fan\":\"%s\",\"a2_power\":\"%s\"}"
        "}",
        st.timestamp_sec,
        st.mode,
        st.model,
        st.protocol,
        st.allow_write ? "true" : "false",
        st.cycle_id,
        st.cycle,
        st.roast_clock,
        st.minutes,
        st.seconds,
        st.drum_temp_ibts_c,
        st.drum_ror_c_min,
        st.bean_temp_c,
        st.bean_ror_c_min,
        st.exhaust_temp_c,
        st.inlet_temp_c,
        st.hot_air_temp_c,
        st.ibts_ambient_c,
        st.heater_power,
        st.fan_speed,
        st.drum_speed,
        st.fan_rpm,
        st.drum_rpm,
        st.ibts_fan_rpm,
        st.induction_fan_rpm,
        st.voltage_v,
        st.current_a,
        st.active_power_w,
        st.power_setpoint_w,
        st.line_freq_hz,
        st.igbt_temp1_c,
        st.igbt_temp2_c,
        st.energy_used_wh,
        st.diff_pressure_pa,
        st.pressure_hpa,
        st.humidity_pct,
        st.crack_count,
        st.error_counts,
        st.room_temp_c,
        st.room_humidity_pct,
        st.battery_v,
        st.battery_pct,
        st.roast_stage,
        st.roast_stage_short,
        st.roast_stage_desc,
        st.roast_stage_color,
        st.roast_progress_pct,
        st.raw_pkt1_hex,
        st.raw_pkt2_hex,
        st.raw_a0_hex,
        st.raw_a1_hex,
        st.raw_a2_hex
    );
    return std::string(buf);
}

} // namespace bulletspy
