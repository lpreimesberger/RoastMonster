#pragma once
/**
 * @file wifi_provision.h
 * @brief Zero-Cable Wi-Fi Provisioning, MicroSD Config Loader, Captive Portal,
 *        mDNS (bulletspy.local), OTA Updater, and MicroSD Roast CSV Logger.
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "bullet_protocol.h"
#include "pins_rlcd42.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#endif

namespace bulletspy {

struct DeviceConfig {
    char  wifi_ssid[64]     = {0};
    char  wifi_password[64] = {0};
    char  hostname[32]      = "bulletspy";
    bool  allow_write       = true;
    float target_drop_c     = 210.0f;
};

static constexpr const char* CAPTIVE_PORTAL_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BulletSpy S3 // Setup</title>
<style>
  body { background:#090d16; color:#f1f5f9; font-family:system-ui,sans-serif; margin:0; padding:24px; }
  .card { max-width:420px; margin:0 auto; background:#111827; border:1px solid #1f2937; border-radius:12px; padding:24px; }
  h1 { font-size:20px; margin:0 0 6px; color:#f97316; }
  p { font-size:13px; color:#9ca3af; margin:0 0 18px; }
  label { display:block; font-size:12px; font-weight:600; color:#cbd5e1; margin:12px 0 4px; text-transform:uppercase; }
  input[type=text], input[type=password], input[type=number] {
    width:100%; box-sizing:border-box; padding:10px 12px; border-radius:8px;
    border:1px solid #374151; background:#030712; color:#fff; font-size:14px;
  }
  .row { display:flex; align-items:center; gap:8px; margin-top:14px; }
  button {
    width:100%; margin-top:20px; padding:12px; background:#f97316; color:#000;
    font-weight:700; border:none; border-radius:8px; font-size:15px; cursor:pointer;
  }
  .link { display:block; text-align:center; margin-top:14px; color:#38bdf8; font-size:13px; text-decoration:none; }
</style>
</head>
<body>
  <div class="card">
    <h1>BulletSpy S3 // Wi-Fi & Roast Setup</h1>
    <p>Configure your Waveshare ESP32-S3-RLCD-4.2 unit. Settings persist in internal NVS flash.</p>
    <form method="POST" action="/api/wifi">
      <label>Wi-Fi Network (SSID)</label>
      <input type="text" name="ssid" placeholder="MyRoasteryWiFi" required>
      <label>Wi-Fi Password</label>
      <input type="password" name="password" placeholder="••••••••">
      <label>mDNS Hostname (.local)</label>
      <input type="text" name="hostname" value="bulletspy">
      <label>Default Target Drop Temp (°C)</label>
      <input type="number" name="target_drop_c" value="210" min="180" max="240">
      <div class="row">
        <input type="checkbox" id="aw" name="allow_write" value="1" checked>
        <label for="aw" style="margin:0">Enable USB Write Control (Heater/Fan/Drum/PRS)</label>
      </div>
      <button type="submit">Save & Connect to Wi-Fi</button>
    </form>
    <a class="link" href="/dashboard">Skip Wi-Fi & Open Live Dashboard Directly &rarr;</a>
  </div>
</body>
</html>)HTML";

class ProvisionManager {
public:
#ifdef ARDUINO
    bool begin() {
        mount_sdcard();
        load_nvs();
        if (sd_mounted_) {
            import_sdcard_config();
        }
        bool connected = connect_sta_or_fallback_ap();
        setup_ota();
        return connected;
    }

    void loop() {
        if (ap_mode_active_) {
            dns_server_.processNextRequest();
        }
        ArduinoOTA.handle();
    }

    /**
     * @brief Mounts the Waveshare ESP32-S3-RLCD-4.2 MicroSD slot in 1-bit SD_MMC mode.
     *        Pins: CLK=21, CMD=38, D0=39.
     */
    bool mount_sdcard() {
        if (!SD_MMC.setPins(rlcd42::PIN_SDMMC_CLK, rlcd42::PIN_SDMMC_CMD, rlcd42::PIN_SDMMC_D0)) {
            sd_mounted_ = false;
            return false;
        }
        sd_mounted_ = SD_MMC.begin("/sdcard", true); // 1-bit mode = true
        if (sd_mounted_ && !SD_MMC.exists("/roasts")) {
            SD_MMC.mkdir("/roasts");
        }
        return sd_mounted_;
    }

    void load_nvs() {
        prefs_.begin("bulletspy", true);
        String ssid = prefs_.getString("ssid", "");
        String pass = prefs_.getString("pass", "");
        String host = prefs_.getString("host", "bulletspy");
        config_.allow_write   = prefs_.getBool("write", true);
        config_.target_drop_c = prefs_.getFloat("drop_c", 210.0f);
        prefs_.end();

        std::snprintf(config_.wifi_ssid, sizeof(config_.wifi_ssid), "%s", ssid.c_str());
        std::snprintf(config_.wifi_password, sizeof(config_.wifi_password), "%s", pass.c_str());
        std::snprintf(config_.hostname, sizeof(config_.hostname), "%s", host.c_str());
    }

    void save_nvs(const char* ssid, const char* pass, const char* host, bool allow_write, float drop_c) {
        std::snprintf(config_.wifi_ssid, sizeof(config_.wifi_ssid), "%s", ssid ? ssid : "");
        std::snprintf(config_.wifi_password, sizeof(config_.wifi_password), "%s", pass ? pass : "");
        if (host && host[0]) {
            std::snprintf(config_.hostname, sizeof(config_.hostname), "%s", host);
        }
        config_.allow_write   = allow_write;
        config_.target_drop_c = drop_c;

        prefs_.begin("bulletspy", false);
        prefs_.putString("ssid", config_.wifi_ssid);
        prefs_.putString("pass", config_.wifi_password);
        prefs_.putString("host", config_.hostname);
        prefs_.putBool("write", config_.allow_write);
        prefs_.putFloat("drop_c", config_.target_drop_c);
        prefs_.end();
    }

    /**
     * @brief Checks for /config.json on the MicroSD card and syncs into NVS flash.
     */
    bool import_sdcard_config() {
        if (!sd_mounted_ || !SD_MMC.exists("/config.json")) {
            return false;
        }
        File f = SD_MMC.open("/config.json", FILE_READ);
        if (!f) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;

        const char* ssid = doc["wifi_ssid"] | config_.wifi_ssid;
        const char* pass = doc["wifi_password"] | config_.wifi_password;
        const char* host = doc["hostname"] | config_.hostname;
        bool allow_w     = doc["allow_write"] | config_.allow_write;
        float drop_c     = doc["target_drop_c"] | config_.target_drop_c;

        save_nvs(ssid, pass, host, allow_w, drop_c);
        return true;
    }

    bool connect_sta_or_fallback_ap() {
        WiFi.setHostname(config_.hostname);
        if (std::strlen(config_.wifi_ssid) > 0) {
            WiFi.mode(WIFI_STA);
            WiFi.begin(config_.wifi_ssid, config_.wifi_password);
            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - start) < 8500) {
                delay(200);
            }
            if (WiFi.status() == WL_CONNECTED) {
                ap_mode_active_ = false;
                MDNS.begin(config_.hostname);
                MDNS.addService("http", "tcp", 80);
                std::snprintf(status_label_, sizeof(status_label_), "%s", WiFi.localIP().toString().c_str());
                return true;
            }
        }

        // Fallback: Start SoftAP + Captive Portal DNS server
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("BulletSpy-Setup");
        dns_server_.start(53, "*", WiFi.softAPIP());
        ap_mode_active_ = true;
        std::snprintf(status_label_, sizeof(status_label_), "AP:192.168.4.1");
        return false;
    }

    void setup_ota() {
        ArduinoOTA.setHostname(config_.hostname);
        ArduinoOTA.begin();
    }

    /**
     * @brief Appends a 1Hz telemetry row to /roasts/roast_batch.csv on MicroSD during active roasts.
     */
    void log_roast_sample_to_sd(const BulletState& st) {
        if (!sd_mounted_ || st.cycle_id != 0x06) return;
        File f = SD_MMC.open("/roasts/latest_roast.csv", FILE_APPEND);
        if (!f) return;
        if (f.size() == 0) {
            f.println("roast_clock,bean_temp_c,drum_ibts_c,bean_ror_c_min,heater,fan,drum,power_w,stage");
        }
        char line[160];
        std::snprintf(line, sizeof(line), "%s,%.1f,%.1f,%.1f,%u,%u,%u,%.1f,%s",
                      st.roast_clock, st.bean_temp_c, st.drum_temp_ibts_c,
                      st.bean_ror_c_min, st.heater_power, st.fan_speed,
                      st.drum_speed, st.active_power_w, st.roast_stage_short);
        f.println(line);
        f.close();
    }
#endif

    const DeviceConfig& config() const { return config_; }
    const char* status_label() const { return status_label_; }
    bool is_ap_setup_mode() const { return ap_mode_active_; }
    void dismiss_ap_banner() { ap_mode_active_ = false; }

private:
    DeviceConfig config_;
    bool sd_mounted_     = false;
    bool ap_mode_active_ = false;
    char status_label_[32] = "OFFLINE";
#ifdef ARDUINO
    Preferences prefs_;
    DNSServer dns_server_;
#endif
};

} // namespace bulletspy
