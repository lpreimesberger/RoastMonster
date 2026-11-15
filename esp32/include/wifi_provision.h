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
  body { background:#090d16; color:#f1f5f9; font-family:system-ui,sans-serif; margin:0; padding:18px; }
  .card { max-width:440px; margin:0 auto; background:#111827; border:1px solid #1f2937; border-radius:12px; padding:20px; }
  h1 { font-size:19px; margin:0 0 4px; color:#f97316; }
  p { font-size:12px; color:#9ca3af; margin:0 0 14px; line-height:1.4; }
  label { display:block; font-size:11px; font-weight:700; color:#cbd5e1; margin:12px 0 4px; text-transform:uppercase; letter-spacing:0.04em; }
  input[type=text], input[type=password], input[type=number] {
    width:100%; box-sizing:border-box; padding:10px 12px; border-radius:8px;
    border:1px solid #374151; background:#030712; color:#fff; font-size:15px;
  }
  .row { display:flex; align-items:center; gap:8px; margin-top:12px; }
  button {
    width:100%; margin-top:18px; padding:12px; background:#f97316; color:#000;
    font-weight:700; border:none; border-radius:8px; font-size:15px; cursor:pointer;
  }
  .btn-sec {
    margin-top:6px; padding:7px 10px; background:#1f2937; color:#e2e8f0;
    font-size:12px; font-weight:600; border:1px solid #374151; border-radius:6px; width:auto;
  }
  .net-list { display:flex; flex-direction:column; gap:6px; margin-top:6px; max-height:160px; overflow-y:auto; }
  .net-item {
    display:flex; justify-content:space-between; align-items:center;
    padding:8px 10px; background:#030712; border:1px solid #1f2937; border-radius:6px;
    font-size:13px; color:#f8fafc; cursor:pointer;
  }
  .net-item:hover, .net-item:active { border-color:#f97316; background:#1e293b; }
  .net-meta { font-size:11px; color:#94a3b8; }
  .status-box {
    padding:10px 12px; border-radius:8px; font-size:12px; margin-bottom:12px;
    background:#1e1b4b; border:1px solid #4338ca; color:#e0e7ff; display:none;
  }
  .status-err { background:#450a0a; border-color:#dc2626; color:#fecaca; }
  .link { display:block; text-align:center; margin-top:14px; color:#38bdf8; font-size:13px; text-decoration:none; }
</style>
</head>
<body>
  <div class="card">
    <h1>BulletSpy S3 // Wi-Fi & Roast Setup</h1>
    <p>ESP32-S3 connects to <b>2.4 GHz</b> Wi-Fi networks. Tap a network below or type your SSID.</p>
    <div id="statusBox" class="status-box"></div>

    <div style="display:flex;justify-content:space-between;align-items:center">
      <label style="margin:0">Available 2.4 GHz Networks (Tap to Select)</label>
      <button type="button" class="btn-sec" onclick="loadScan(true)">Rescan</button>
    </div>
    <div id="netList" class="net-list">
      <div class="net-item"><span class="net-meta">Scanning 2.4 GHz band...</span></div>
    </div>

    <form id="wifiForm" method="GET" action="/api/wifi" onsubmit="return submitWifi(event)">
      <label>Wi-Fi Network (2.4 GHz SSID)</label>
      <input type="text" id="ssid" name="ssid" placeholder="Select above or type SSID"
             autocapitalize="none" autocorrect="off" autocomplete="off" spellcheck="false" required>
      <label>Wi-Fi Password</label>
      <div style="position:relative">
        <input type="password" id="pass" name="password" placeholder="••••••••"
               autocapitalize="none" autocorrect="off" autocomplete="off" spellcheck="false">
      </div>
      <div class="row" style="margin-top:6px">
        <input type="checkbox" id="showpw" onchange="document.getElementById('pass').type=this.checked?'text':'password'">
        <label for="showpw" style="margin:0;font-weight:400;text-transform:none">Show password characters</label>
      </div>
      <label>mDNS Hostname (.local)</label>
      <input type="text" id="host" name="hostname" value="bulletspy"
             autocapitalize="none" autocorrect="off" spellcheck="false">
      <label>Default Target Drop Temp (°C)</label>
      <input type="number" id="drop" name="target_drop_c" value="210" min="180" max="240">
      <div class="row">
        <input type="checkbox" id="aw" name="allow_write" value="1" checked>
        <label for="aw" style="margin:0">Enable USB Write Control (Heater/Fan/Drum/PRS)</label>
      </div>
      <button type="submit" id="saveBtn">Save & Connect to Wi-Fi</button>
    </form>
    <a class="link" href="/dashboard">Skip Wi-Fi & Open Live Dashboard Directly &rarr;</a>

    <hr style="border:none;border-top:1px solid #1f2937;margin:22px 0">
    <h1>Firmware Update (OTA)</h1>
    <p>Guard: Verifies ESP32-S3 Chip-ID (<code>0x0009</code>), SHA-256 manifest digest, &amp; dual-partition rollback.</p>
    <div id="otaStatus" class="status-box"></div>

    <div style="display:flex;gap:8px;margin-top:8px">
      <button type="button" class="btn-sec" style="flex:1;margin:0;padding:10px" onclick="checkGithubOta()">Check GitHub Release</button>
      <button type="button" id="ghFlashBtn" class="btn-sec" style="flex:1;margin:0;padding:10px;background:#059669;color:#fff;border-color:#10b981;display:none" onclick="pullGithubOta()">Install GitHub Update</button>
    </div>

    <label style="margin-top:16px">Or Upload Local Firmware (.bin)</label>
    <input type="file" id="otaFile" accept=".bin" style="width:100%;font-size:13px;color:#cbd5e1;margin-top:4px">
    <label>Optional Expected SHA-256 Digest (64 hex chars)</label>
    <input type="text" id="otaSha" placeholder="Leave blank or paste SHA-256 to enforce"
           autocapitalize="none" autocorrect="off" spellcheck="false" style="font-family:monospace;font-size:12px">
    <div id="otaProgWrap" style="display:none;margin-top:10px;background:#030712;border:1px solid #374151;border-radius:6px;overflow:hidden;height:18px">
      <div id="otaProgBar" style="width:0%;height:100%;background:#10b981;transition:width 0.15s"></div>
    </div>
    <button type="button" id="uploadBtn" style="background:#38bdf8;margin-top:12px" onclick="uploadLocalOta()">Upload &amp; Flash .bin</button>
  </div>
<script>
function showOtaMsg(msg, isErr) {
  const b = document.getElementById('otaStatus');
  b.style.display = 'block';
  b.className = isErr ? 'status-box status-err' : 'status-box';
  b.textContent = msg;
}
function checkGithubOta(pollCount) {
  const pc = pollCount || 0;
  if (pc === 0) showOtaMsg('Checking github.com/lpreimesberger/RoastMonster/releases/latest...', false);
  fetch('/api/ota/check' + (pc === 0 ? '?refresh=1' : ''))
    .then(r => r.json())
    .then(d => {
      if (d.checking) {
        if (pc < 10) setTimeout(() => checkGithubOta(pc + 1), 1200);
        else showOtaMsg('GitHub check timed out.', true);
        return;
      }
      if (!d.ok) { showOtaMsg(d.error || 'Check failed', true); return; }
      const msg = 'Current: v' + d.current_version + ' | Latest GitHub: v' + d.latest_version +
                  (d.update_available ? ' (UPDATE AVAILABLE!)' : ' (Up to date)');
      showOtaMsg(msg, false);
      if (d.bin_url) document.getElementById('ghFlashBtn').style.display = 'block';
    })
    .catch(e => showOtaMsg('Network error checking GitHub: ' + e, true));
}
function pullGithubOta() {
  const btn = document.getElementById('ghFlashBtn');
  btn.disabled = true;
  showOtaMsg('Downloading & verifying firmware from GitHub Releases... Do not unplug.', false);
  fetch('/api/ota/github', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      if (!d.ok) { btn.disabled = false; showOtaMsg(d.detail || 'Update failed', true); return; }
      showOtaMsg(d.detail + ' Rebooting in 3 seconds...', false);
      setTimeout(() => { location.href = '/'; }, 5000);
    })
    .catch(e => { btn.disabled = false; showOtaMsg('GitHub OTA error: ' + e, true); });
}
function uploadLocalOta() {
  const f = document.getElementById('otaFile').files[0];
  if (!f) { showOtaMsg('Please select a firmware .bin file first.', true); return; }
  const sha = document.getElementById('otaSha').value.trim();
  const fd = new FormData();
  fd.append('firmware', f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota/upload' + (sha ? ('?sha256=' + encodeURIComponent(sha)) : ''), true);
  document.getElementById('otaProgWrap').style.display = 'block';
  document.getElementById('uploadBtn').disabled = true;
  xhr.upload.onprogress = e => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('otaProgBar').style.width = pct + '%';
      showOtaMsg('Uploading & verifying ESP32-S3 image: ' + pct + '%', false);
    }
  };
  xhr.onload = () => {
    document.getElementById('uploadBtn').disabled = false;
    try {
      const d = JSON.parse(xhr.responseText);
      if (d.ok) {
        showOtaMsg('Verified & Flashed! SHA-256: ' + d.sha256.substring(0, 16) + '... Rebooting!', false);
        setTimeout(() => { location.href = '/'; }, 5000);
      } else {
        showOtaMsg('Rejected: ' + d.detail, true);
      }
    } catch (_) {
      showOtaMsg('Upload response: ' + xhr.responseText, xhr.status !== 200);
    }
  };
  xhr.onerror = () => {
    document.getElementById('uploadBtn').disabled = false;
    showOtaMsg('Upload connection interrupted.', true);
  };
  xhr.send(fd);
}
function pickSsid(s) {
  document.getElementById('ssid').value = s;
  document.getElementById('pass').focus();
}
function submitWifi(e) {
  e.preventDefault();
  const btn = document.getElementById('saveBtn');
  btn.textContent = 'Saving & Connecting...';
  btn.disabled = true;
  const q = new URLSearchParams({
    ssid: document.getElementById('ssid').value,
    password: document.getElementById('pass').value,
    hostname: document.getElementById('host').value,
    target_drop_c: document.getElementById('drop').value
  });
  if (document.getElementById('aw').checked) q.set('allow_write', '1');
  fetch('/api/wifi?' + q.toString(), { method: 'POST' })
    .then(r => r.text())
    .then(html => { document.open(); document.write(html); document.close(); })
    .catch(() => { document.getElementById('wifiForm').submit(); });
  return false;
}
function loadScan(rescan) {
  const nl = document.getElementById('netList');
  if (rescan) nl.innerHTML = '<div class="net-item"><span class="net-meta">Rescanning 2.4 GHz...</span></div>';
  fetch('/api/scan' + (rescan ? '?refresh=1' : ''))
    .then(r => r.json())
    .then(d => {
      const sb = document.getElementById('statusBox');
      if (d.last_error && d.last_error.length > 0) {
        sb.style.display = 'block';
        sb.className = 'status-box status-err';
        sb.textContent = 'Last Wi-Fi attempt: ' + d.last_error;
      } else if (d.saved_ssid && d.saved_ssid.length > 0) {
        sb.style.display = 'block';
        sb.className = 'status-box';
        sb.textContent = 'Saved SSID: ' + d.saved_ssid;
      }
      if (d.saved_ssid && !document.getElementById('ssid').value) {
        document.getElementById('ssid').value = d.saved_ssid;
      }
      if (d.hostname) document.getElementById('host').value = d.hostname;
      if (d.target_drop_c) document.getElementById('drop').value = d.target_drop_c;
      nl.innerHTML = '';
      if (!d.networks || d.networks.length === 0) {
        nl.innerHTML = '<div class="net-item"><span class="net-meta">No 2.4 GHz networks found. Tap Rescan.</span></div>';
        return;
      }
      d.networks.forEach(n => {
        const div = document.createElement('div');
        div.className = 'net-item';
        div.onclick = () => pickSsid(n.ssid);
        div.innerHTML = '<b>' + n.ssid + '</b><span class="net-meta">Ch ' + n.channel + ' (' + n.rssi + ' dBm)</span>';
        nl.appendChild(div);
      });
    })
    .catch(() => {});
}
loadScan(false);
</script>
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

    static void trim_inplace(char* s) {
        if (!s) return;
        size_t len = std::strlen(s);
        while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
            s[--len] = '\0';
        }
        size_t start = 0;
        while (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n') {
            start++;
        }
        if (start > 0) {
            std::memmove(s, s + start, len - start + 1);
        }
    }

    void save_nvs(const char* ssid, const char* pass, const char* host, bool allow_write, float drop_c) {
        std::snprintf(config_.wifi_ssid, sizeof(config_.wifi_ssid), "%s", ssid ? ssid : "");
        trim_inplace(config_.wifi_ssid);
        std::snprintf(config_.wifi_password, sizeof(config_.wifi_password), "%s", pass ? pass : "");
        if (host && host[0]) {
            std::snprintf(config_.hostname, sizeof(config_.hostname), "%s", host);
            trim_inplace(config_.hostname);
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

    void refresh_scan_cache(bool check_saved_ssid_case = false, bool* out_found_in_scan = nullptr) {
        int n = WiFi.scanNetworks(false, false);
        bool found = false;
        if (check_saved_ssid_case && std::strlen(config_.wifi_ssid) > 0 && n > 0) {
            for (int i = 0; i < n; ++i) {
                String s = WiFi.SSID(i);
                if (s == config_.wifi_ssid) {
                    found = true;
                    break;
                }
            }
            // Auto-fix phone keyboard capitalization (e.g., "Milfgard" -> "milfgard")
            if (!found) {
                for (int i = 0; i < n; ++i) {
                    String s = WiFi.SSID(i);
                    if (s.equalsIgnoreCase(config_.wifi_ssid)) {
                        Serial.printf("[BulletSpy Wi-Fi] Auto-corrected SSID case '%s' -> '%s'\n",
                                      config_.wifi_ssid, s.c_str());
                        save_nvs(s.c_str(), config_.wifi_password, config_.hostname,
                                 config_.allow_write, config_.target_drop_c);
                        found = true;
                        break;
                    }
                }
            }
        }
        if (out_found_in_scan) *out_found_in_scan = found;

        JsonDocument doc;
        doc["saved_ssid"]    = config_.wifi_ssid;
        doc["hostname"]      = config_.hostname;
        doc["target_drop_c"] = config_.target_drop_c;
        doc["last_error"]    = last_error_;
        JsonArray arr = doc["networks"].to<JsonArray>();

        for (int i = 0; i < n; ++i) {
            String s = WiFi.SSID(i);
            if (s.length() == 0) continue;
            // Deduplicate SSIDs (keep strongest RSSI)
            bool dup = false;
            for (JsonVariant v : arr) {
                if (s == v["ssid"].as<const char*>()) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"]    = s;
            obj["rssi"]    = WiFi.RSSI(i);
            obj["channel"] = WiFi.channel(i);
        }
        cached_scan_json_ = "";
        serializeJson(doc, cached_scan_json_);
        WiFi.scanDelete();
    }

    String get_scan_json(bool force_refresh) {
        if (force_refresh || cached_scan_json_.length() == 0) {
            refresh_scan_cache(false, nullptr);
        } else {
            // Ensure latest saved_ssid and last_error_ are reflected
            JsonDocument doc;
            if (deserializeJson(doc, cached_scan_json_) == DeserializationError::Ok) {
                doc["saved_ssid"] = config_.wifi_ssid;
                doc["last_error"] = last_error_;
                cached_scan_json_ = "";
                serializeJson(doc, cached_scan_json_);
            }
        }
        return cached_scan_json_;
    }

    bool connect_sta_or_fallback_ap() {
        WiFi.persistent(false);
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        WiFi.setAutoReconnect(true);
        WiFi.setHostname(config_.hostname);

        // Always run an active 2.4GHz probe scan first: wakes up band-steered tri-band APs
        // (like cheerleadernet) and auto-corrects phone keyboard SSID capitalization.
        bool found_in_scan = false;
        refresh_scan_cache(true, &found_in_scan);

        if (std::strlen(config_.wifi_ssid) > 0) {
            Serial.printf("[BulletSpy Wi-Fi] Connecting to 2.4GHz SSID '%s' (found_in_scan=%d)...\n",
                          config_.wifi_ssid, found_in_scan ? 1 : 0);
            WiFi.begin(config_.wifi_ssid, config_.wifi_password);
            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - start) < 16000) {
                delay(250);
            }
            if (WiFi.status() == WL_CONNECTED) {
                WiFi.setSleep(WIFI_PS_NONE);
                last_error_[0]       = '\0';
                ap_mode_active_      = false;
                ap_banner_dismissed_ = true;
                MDNS.begin(config_.hostname);
                MDNS.addService("http", "tcp", 80);
                std::snprintf(status_label_, sizeof(status_label_), "%s", WiFi.localIP().toString().c_str());
                Serial.printf("[BulletSpy Wi-Fi] Connected! IP=%s\n", status_label_);
                return true;
            }

            wl_status_t st = WiFi.status();
            if (!found_in_scan) {
                std::snprintf(last_error_, sizeof(last_error_),
                              "SSID '%s' not found on 2.4 GHz (5 GHz-only?). Select a 2.4 GHz network below.",
                              config_.wifi_ssid);
            } else if (st == WL_CONNECT_FAILED) {
                std::snprintf(last_error_, sizeof(last_error_),
                              "Authentication failed for '%s'. Check Wi-Fi password.",
                              config_.wifi_ssid);
            } else {
                std::snprintf(last_error_, sizeof(last_error_),
                              "Timed out connecting to '%s' (status=%d). Check password/signal.",
                              config_.wifi_ssid, (int)st);
            }
            Serial.printf("[BulletSpy Wi-Fi] %s\n", last_error_);
            WiFi.disconnect(true);
            delay(100);
        }

        // Fallback: Start SoftAP + Captive Portal DNS server
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("BulletSpy-Setup");
        delay(100);
        dns_server_.setErrorReplyCode(DNSReplyCode::NoError);
        dns_server_.start(53, "*", WiFi.softAPIP());
        ap_mode_active_      = true;
        ap_banner_dismissed_ = false;
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
    bool show_ap_banner() const { return ap_mode_active_ && !ap_banner_dismissed_; }
    void dismiss_ap_banner() { ap_banner_dismissed_ = true; }
    bool is_sd_mounted() const { return sd_mounted_; }

private:
    DeviceConfig config_;
    bool sd_mounted_          = false;
    bool ap_mode_active_      = false;
    bool ap_banner_dismissed_ = false;
    char status_label_[32]    = "OFFLINE";
    char last_error_[160]     = {0};
#ifdef ARDUINO
    String cached_scan_json_;
    Preferences prefs_;
    DNSServer dns_server_;
#endif
};

} // namespace bulletspy
