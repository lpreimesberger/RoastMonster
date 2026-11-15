/**
 * @file main.cpp
 * @brief BulletSpy S3 Firmware for the Waveshare ESP32-S3-RLCD-4.2
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 *
 * Features:
 *  - 4Hz (250ms) Aillio Bullet R2 & R1/IBTS USB OTG Host Poller + Binary Unpacker
 *  - Automatic Simulated Binary Frame Mode when unplugged from roaster
 *  - Real-time 400x300 Sitronix ST7305 Paper-Like RLCD Dashboard (2x4 block GRAM)
 *  - Onboard SHTC3 Ambient Temp/Humidity + 18650 Battery % + ES8311 I2S Audio Chimes
 *  - Zero-Cable Wi-Fi Setup: MicroSD /config.json + SoftAP Captive Portal + NVS Flash
 *  - Embedded Gzipped Dark-Mode Web Dashboard (11.8 KB PROGMEM) + /ws WebSocket Stream
 *  - Automatic MicroSD Roast Batch CSV Logging (/roasts/latest_roast.csv)
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <soc/usb_serial_jtag_struct.h>

#include "audio_alerts.h"
#include "bullet_protocol.h"
#include "dashboard_html.h"
#include "ota_updater.h"
#include "pins_rlcd42.h"
#include "st7305_rlcd.h"
#include "usb_bullet_host.h"
#include "wifi_provision.h"

using namespace bulletspy;

static ST7305Display    g_display;
static BoardPeripherals g_peripherals;
static ProvisionManager g_provision;
static BulletUsbHost    g_bullet;
static OtaManager       g_ota;

static AsyncWebServer   g_server(80);
static AsyncWebSocket   g_ws("/ws");

static uint32_t g_last_poll_ms   = 0;
static uint32_t g_last_sensor_ms = 0;
static uint32_t g_last_sd_log_ms = 0;
static bool     g_last_key_state = HIGH;

static void send_gzipped_dashboard(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(
        200, "text/html; charset=utf-8", DASHBOARD_HTML_GZ, DASHBOARD_HTML_GZ_LEN
    );
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
}

static void on_ws_event(AsyncWebSocket* /*server*/, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        std::string snap = state_to_json(g_bullet.snapshot());
        client->text(snap.c_str(), snap.size());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len) == DeserializationError::Ok) {
                const char* action = doc["action"] | "";
                bool has_val = !doc["value"].isNull();
                int val = doc["value"] | 0;
                std::vector<std::string> pkts;
                g_bullet.dispatch_command(action, val, has_val, pkts);
            }
        }
    }
}

static volatile uint32_t g_pending_restart_ms = 0;
static volatile bool     g_pending_gh_check   = false;
static volatile bool     g_pending_gh_ota     = false;
static String            g_gh_check_cache     = "";

static void setup_http_routes() {
    g_ws.onEvent(on_ws_event);
    g_server.addHandler(&g_ws);

    // Register specific routes FIRST so ESPAsyncWebServer prefix-matching on "/" never shadows them
    g_server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html; charset=utf-8", CAPTIVE_PORTAL_HTML);
    });

    g_server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest* req) {
        send_gzipped_dashboard(req);
    });

    g_server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool refresh = req->hasParam("refresh");
        String scan_json = g_provision.get_scan_json(refresh);
        req->send(200, "application/json", scan_json);
    });

    // Non-blocking GitHub Release OTA check endpoint
    g_server.on("/api/ota/check", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (req->hasParam("refresh") || g_gh_check_cache.length() == 0) {
            g_pending_gh_check = true;
            req->send(200, "application/json", "{\"checking\":true}");
            return;
        }
        req->send(200, "application/json", g_gh_check_cache);
    });

    // Trigger GitHub Release HTTPS download + SHA-256/ECDSA verified flash
    g_server.on("/api/ota/github", HTTP_POST, [](AsyncWebServerRequest* req) {
        g_pending_gh_ota = true;
        req->send(200, "application/json",
                  "{\"ok\":true,\"detail\":\"Downloading & verifying GitHub Release on ESP32-S3 (watch RLCD screen)...\"}");
    });

    // Browser Multipart .bin Firmware Upload (/api/ota/upload)
    g_server.on(
        "/api/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            JsonDocument res;
            res["ok"]     = g_ota.browser_upload_ok();
            res["sha256"] = g_ota.last_sha256_hex();
            res["detail"] = g_ota.browser_upload_ok() ? "Firmware verified and flashed" : g_ota.browser_upload_error();
            String out;
            serializeJson(res, out);
            req->send(g_ota.browser_upload_ok() ? 200 : 400, "application/json", out);
            if (g_ota.browser_upload_ok()) {
                g_pending_restart_ms = millis() + 900;
            }
        },
        [](AsyncWebServerRequest* req, const String& /*filename*/, size_t index, uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                g_ota.begin_browser_upload();
            }
            if (len > 0) {
                g_ota.write_browser_chunk(data, len, index);
            }
            if (final) {
                String exp_sha = req->hasParam("sha256", false) ? req->getParam("sha256", false)->value() : "";
                g_ota.finish_browser_upload(exp_sha);
            }
        }
    );

    g_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        std::string json = state_to_json(g_bullet.snapshot());
        req->send(200, "application/json", json.c_str());
    });

    // Wi-Fi & device configuration handler (supports both GET query params and POST body params)
    g_server.on("/api/wifi", HTTP_ANY, [](AsyncWebServerRequest* req) {
        auto get_p = [req](const char* name, const char* def_val) -> String {
            if (req->hasParam(name, false)) return req->getParam(name, false)->value();
            if (req->hasParam(name, true))  return req->getParam(name, true)->value();
            return String(def_val);
        };
        String ssid = get_p("ssid", "");
        String pass = get_p("password", "");
        String host = get_p("hostname", "bulletspy");
        bool allow_w = req->hasParam("allow_write", false) || req->hasParam("allow_write", true);
        float drop_c = get_p("target_drop_c", "210").toFloat();
        if (drop_c < 150.0f || drop_c > 250.0f) drop_c = 210.0f;

        g_provision.save_nvs(ssid.c_str(), pass.c_str(), host.c_str(), allow_w, drop_c);
        g_bullet.set_allow_write(allow_w);
        g_bullet.set_target_drop(drop_c);

        req->send(200, "text/html; charset=utf-8",
                  "<html><body style='background:#090d16;color:#10b981;font-family:sans-serif;padding:32px'>"
                  "<h2>Saved! Rebooting BulletSpy S3 and joining 2.4 GHz Wi-Fi...</h2>"
                  "<p>Connecting to <b>" + String(g_provision.config().wifi_ssid) + "</b>...</p>"
                  "<p>Check the 4.2&quot; RLCD screen top-right badge for the assigned IP address, or open "
                  "<b>http://" + String(g_provision.config().hostname) + ".local</b> in ~15 seconds.</p></body></html>");
        g_pending_restart_ms = millis() + 800;
    });

    // JSON command POST endpoint (/api/command)
    g_server.on(
        "/api/command", HTTP_POST,
        [](AsyncWebServerRequest* /*req*/) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t /*index*/, size_t /*total*/) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"ok\":false,\"detail\":\"Invalid JSON\"}");
                return;
            }
            const char* action = doc["action"] | "";
            bool has_val = !doc["value"].isNull();
            int val = doc["value"] | 0;

            std::vector<std::string> pkts;
            if (!g_bullet.dispatch_command(action, val, has_val, pkts)) {
                req->send(403, "application/json",
                          "{\"ok\":false,\"detail\":\"Write mode disabled or invalid command\"}");
                return;
            }

            JsonDocument res;
            res["ok"]     = true;
            res["action"] = action;
            if (has_val) res["value"] = val;
            JsonArray arr = res["queued_packets_hex"].to<JsonArray>();
            for (const auto& h : pkts) arr.add(h);

            String out;
            serializeJson(res, out);
            req->send(200, "application/json", out);
        }
    );

    // Root route registered after specific routes
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (g_provision.is_ap_setup_mode()) {
            req->send(200, "text/html; charset=utf-8", CAPTIVE_PORTAL_HTML);
        } else {
            send_gzipped_dashboard(req);
        }
    });

    // Captive portal catch-all: serve setup HTML directly with 200 OK (no 302 redirect loops!)
    g_server.onNotFound([](AsyncWebServerRequest* req) {
        if (g_provision.is_ap_setup_mode()) {
            req->send(200, "text/html; charset=utf-8", CAPTIVE_PORTAL_HTML);
        } else {
            req->send(404, "text/plain", "Not Found");
        }
    });

    g_server.begin();
}

void setup() {
    Serial.begin(115200);

    // 1. Initialize onboard I2C, SHTC3, ES8311 DAC, and side buttons
    g_peripherals.begin();

    // Escape hatch: Hold the side [KEY] button (GPIO18) at power-on to keep USB Serial debug mode
    bool hold_key_at_boot = (digitalRead(rlcd42::PIN_BTN_KEY) == LOW);
    if (hold_key_at_boot) {
        delay(300);
        Serial.println("[BulletSpy S3] KEY held at boot -> USB Host skipped (Serial Debug Active)");
    }

    // 2. Initialize the 4.2" 400x300 ST7305 Reflective LCD & show Beanzilla Boot Splash
    g_display.begin();
    g_display.render_boot_splash("SCANNING 2.4GHZ & CONNECTING WIFI...");
    g_display.display();
    delay(800);

    // 3. Mount MicroSD (/config.json), load NVS flash, and join Wi-Fi or start SoftAP Captive Portal
    g_provision.begin();

    // 3b. Check for /update.bin on MicroSD card (auto-verifies ESP32-S3 header + SHA-256 and flashes)
    if (g_ota.check_and_apply_sdcard_update(g_provision.is_sd_mounted(), g_display)) {
        g_peripherals.play_tone(1046, 120);
        g_peripherals.play_tone(1318, 180);
        ESP.restart();
    }

    // 4. Start the Aillio Bullet R2/R1 USB OTG Host task (or Simulated mode)
    const auto& cfg = g_provision.config();
    g_bullet.begin(cfg.allow_write, hold_key_at_boot);
    g_bullet.set_target_drop(cfg.target_drop_c);

    // 5. Start Async HTTP + WebSocket Server
    setup_http_routes();

    // Play startup ready chime on ES8311 speaker
    g_peripherals.play_tone(880, 90);
}

void loop() {
    g_provision.loop();
    g_ws.cleanupClients();

    const uint32_t now_ms = millis();

    if (g_pending_restart_ms != 0 && (int32_t)(now_ms - g_pending_restart_ms) >= 0) {
        ESP.restart();
    }

    if (g_pending_gh_check) {
        g_pending_gh_check = false;
        g_gh_check_cache   = g_ota.check_github_release_json();
    }

    if (g_pending_gh_ota) {
        g_pending_gh_ota = false;
        g_display.render_boot_splash("DOWNLOADING & VERIFYING GITHUB OTA...");
        g_display.display();
        String msg;
        if (g_ota.pull_and_flash_from_github(msg)) {
            g_display.render_boot_splash("OTA VERIFIED & COMPLETE! REBOOTING...");
            g_display.display();
            g_peripherals.play_tone(1046, 120);
            g_peripherals.play_tone(1318, 180);
            delay(800);
            ESP.restart();
        } else {
            Serial.printf("[BulletSpy GitHub-OTA] Failed: %s\n", msg.c_str());
        }
    }

    // Only switch USB pins (GPIO19/20) to USB Host Mode when NOT plugged into a PC host
    // (hardware 1kHz USB SOF frame counter is idle) and NOT in SoftAP setup mode.
    if (now_ms > 25000 && !g_provision.is_ap_setup_mode()) {
        uint32_t f0 = USB_SERIAL_JTAG.fram_num.sof_frame_index;
        delay(3);
        uint32_t f1 = USB_SERIAL_JTAG.fram_num.sof_frame_index;
        if (f0 == f1 && !HWCDC::isPlugged()) {
            g_bullet.start_usb_host_tasks();
        }
    }

    // Side [KEY] button (GPIO18): dismisses AP setup banner on RLCD or advances PRS cycle
    bool key_state = digitalRead(rlcd42::PIN_BTN_KEY);
    if (key_state == LOW && g_last_key_state == HIGH) {
        if (g_provision.show_ap_banner()) {
            g_provision.dismiss_ap_banner();
        } else {
            std::vector<std::string> pkts;
            g_bullet.dispatch_command("prs", 0, false, pkts);
            g_peripherals.play_tone(1046, 60);
        }
    }
    g_last_key_state = key_state;

    // Poll onboard SHTC3 ambient sensor + 18650 battery voltage every 2 seconds
    if ((now_ms - g_last_sensor_ms) >= 2000) {
        g_last_sensor_ms = now_ms;
        float room_t = 24.0f, room_rh = 45.0f, bat_v = 4.10f;
        uint8_t bat_pct = 95;
        g_peripherals.poll_sensors(room_t, room_rh, bat_v, bat_pct);
        g_bullet.update_board_metrics(room_t, room_rh, bat_v, bat_pct);
        if (g_provision.is_ap_setup_mode()) {
            Serial.printf("[BulletSpy AP Diag] %s\n", g_provision.get_scan_json(false).c_str());
        }
    }

    // Main 4Hz (250ms) USB Telemetry Poll + RLCD Render + WebSocket Broadcast
    if ((now_ms - g_last_poll_ms) >= 250) {
        g_last_poll_ms = now_ms;
        double now_sec = now_ms / 1000.0;

        g_bullet.poll_step(now_sec);
        BulletState snap = g_bullet.snapshot();

        // Check audio alarms (Charge Ready, First Crack, Approaching Target, Drop Now)
        g_peripherals.check_roast_alarms(snap);

        // Render and push 15,000-byte frame to the 400x300 ST7305 RLCD (<15ms over 10MHz SPI)
        g_display.render_dashboard(snap, g_provision.status_label(), g_provision.show_ap_banner());
        g_display.display();

        // Broadcast 4Hz JSON snapshot to any connected browser WebSockets
        if (g_ws.count() > 0) {
            std::string payload = state_to_json(snap);
            g_ws.textAll(payload.c_str(), payload.size());
        }

        // Log 1Hz CSV row to MicroSD card during active roasting
        if ((now_ms - g_last_sd_log_ms) >= 1000) {
            g_last_sd_log_ms = now_ms;
            g_provision.log_roast_sample_to_sd(snap);
        }
    }

    delay(5);
}
