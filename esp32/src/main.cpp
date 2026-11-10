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

#include "audio_alerts.h"
#include "bullet_protocol.h"
#include "dashboard_html.h"
#include "pins_rlcd42.h"
#include "st7305_rlcd.h"
#include "usb_bullet_host.h"
#include "wifi_provision.h"

using namespace bulletspy;

static ST7305Display    g_display;
static BoardPeripherals g_peripherals;
static ProvisionManager g_provision;
static BulletUsbHost    g_bullet;

static AsyncWebServer   g_server(80);
static AsyncWebSocket   g_ws("/ws");

static uint32_t g_last_poll_ms   = 0;
static uint32_t g_last_sensor_ms = 0;
static uint32_t g_last_sd_log_ms = 0;
static bool     g_last_key_state = HIGH;

static void send_gzipped_dashboard(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse_P(
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

static void setup_http_routes() {
    g_ws.onEvent(on_ws_event);
    g_server.addHandler(&g_ws);

    // Root route: serves Captive Portal when in AP setup mode, or the live dashboard
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (g_provision.is_ap_setup_mode()) {
            req->send(200, "text/html; charset=utf-8", CAPTIVE_PORTAL_HTML);
        } else {
            send_gzipped_dashboard(req);
        }
    });

    // Direct dashboard route (accessible even in SoftAP mode)
    g_server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest* req) {
        send_gzipped_dashboard(req);
    });

    // Always open Wi-Fi setup page at /setup
    g_server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html; charset=utf-8", CAPTIVE_PORTAL_HTML);
    });

    // Telemetry JSON snapshot
    g_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        std::string json = state_to_json(g_bullet.snapshot());
        req->send(200, "application/json", json.c_str());
    });

    // Wi-Fi & device configuration POST handler
    g_server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* req) {
        String ssid = req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : "";
        String pass = req->hasParam("password", true) ? req->getParam("password", true)->value() : "";
        String host = req->hasParam("hostname", true) ? req->getParam("hostname", true)->value() : "bulletspy";
        bool allow_w = req->hasParam("allow_write", true);
        float drop_c = req->hasParam("target_drop_c", true)
                           ? req->getParam("target_drop_c", true)->value().toFloat()
                           : 210.0f;

        g_provision.save_nvs(ssid.c_str(), pass.c_str(), host.c_str(), allow_w, drop_c);
        g_bullet.set_allow_write(allow_w);
        g_bullet.set_target_drop(drop_c);

        req->send(200, "text/html; charset=utf-8",
                  "<html><body style='background:#090d16;color:#10b981;font-family:sans-serif;padding:32px'>"
                  "<h2>Saved! Rebooting BulletSpy S3 and joining Wi-Fi...</h2>"
                  "<p>Open <b>http://" + host + ".local</b> in a few seconds.</p></body></html>");
        delay(800);
        ESP.restart();
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

    // Captive portal catch-all redirect when in SoftAP setup mode
    g_server.onNotFound([](AsyncWebServerRequest* req) {
        if (g_provision.is_ap_setup_mode()) {
            req->redirect("http://192.168.4.1/");
        } else {
            req->send(404, "text/plain", "Not Found");
        }
    });

    g_server.begin();
}

void setup() {
    // 1. Initialize onboard I2C, SHTC3, ES8311 DAC, and side buttons
    g_peripherals.begin();

    // Escape hatch: Hold the side [KEY] button (GPIO18) at power-on to keep USB Serial debug mode
    bool hold_key_at_boot = (digitalRead(rlcd42::PIN_BTN_KEY) == LOW);
    if (hold_key_at_boot) {
        Serial.begin(115200);
        delay(300);
        Serial.println("[BulletSpy S3] KEY held at boot -> USB Host skipped (Serial Debug Active)");
    }

    // 2. Initialize the 4.2" 400x300 ST7305 Reflective LCD & show Beanzilla Boot Splash
    g_display.begin();
    g_display.render_boot_splash("MOUNTING MICROSD & CONNECTING WIFI...");
    g_display.display();
    delay(1200);

    // 3. Mount MicroSD (/config.json), load NVS flash, and join Wi-Fi or start SoftAP Captive Portal
    g_provision.begin();

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

    // Side [KEY] button (GPIO18): dismisses AP setup banner or advances PRS cycle in write mode
    bool key_state = digitalRead(rlcd42::PIN_BTN_KEY);
    if (key_state == LOW && g_last_key_state == HIGH) {
        if (g_provision.is_ap_setup_mode()) {
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
        g_display.render_dashboard(snap, g_provision.status_label(), g_provision.is_ap_setup_mode());
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
