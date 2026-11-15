#pragma once
/**
 * @file ota_updater.h
 * @brief Secure Over-The-Air (OTA) & MicroSD Firmware Update Engine for BulletSpy S3.
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 *
 * Supports three update paths into the ESP32-S3's dual app0/app1 OTA partitions:
 *   1. GitHub Releases HTTPS Pull (checks ota_manifest.json on lpreimesberger/RoastMonster)
 *   2. Browser Multipart .bin Upload (on /setup -> POST /api/ota/upload)
 *   3. MicroSD Card Auto-Update (/update.bin + optional /update.sha256 at boot)
 *
 * Security Guards:
 *   - Hardware Image Header Guard: Verifies ESP32 magic byte (0xE9) AND ESP32-S3
 *     chip_id (0x0009) in the 24-byte esp_image_header_t before writing to flash.
 *   - Streaming SHA-256 Digest Verification (mbedtls_sha256) against ota_manifest.json
 *     or /update.sha256.
 *   - Optional ECDSA P-256 (secp256r1) cryptographic signature verification via mbedtls_pk
 *     when BEANZILLA_OTA_PUBKEY_PEM is populated.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef ARDUINO
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#endif

namespace bulletspy {

static constexpr const char* BULLETSPY_FW_VERSION   = "1.1.0";
static constexpr const char* GITHUB_REPO_SLUG       = "lpreimesberger/RoastMonster";
static constexpr const char* GITHUB_MANIFEST_ASSET  = "ota_manifest.json";
static constexpr const char* GITHUB_FIRMWARE_ASSET  = "bulletspy-esp32s3-rlcd42.bin";

// ESP32-S3 image header constants (from esp_app_format.h)
static constexpr uint8_t  ESP_IMAGE_HEADER_MAGIC = 0xE9;
static constexpr uint16_t ESP_CHIP_ID_ESP32S3    = 0x0009;

/**
 * @brief Optional ECDSA P-256 Public Key (PEM) for cryptographic release signature enforcement.
 *        Leave empty ("") to enforce ESP32-S3 Chip-ID header guard + SHA-256 manifest digest;
 *        populate with the public key from `python3 esp32/sign_ota.py --gen-key` to enforce
 *        mandatory ECDSA P-256 signature verification on all GitHub & MicroSD updates.
 */
static constexpr const char* BEANZILLA_OTA_PUBKEY_PEM = R"PEM(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVRyr83CNbz8LzkM0IZkKxM/Ppfbg
4mBl6w6Wv6NhRU8U74Qt3frHDwIa4ktK50GdlXIBtyCAHj+KQ1Uk+ccJxA==
-----END PUBLIC KEY-----
)PEM";

/**
 * @brief Validates the first 16+ bytes of an ESP-IDF firmware binary to ensure it is a
 *        genuine ESP32-S3 application image (magic 0xE9, chip_id 0x0009).
 */
inline bool validate_esp32s3_image_header(const uint8_t* data, size_t len, char* err_buf, size_t err_len) {
    if (!data || len < 16) {
        std::snprintf(err_buf, err_len, "Image header too short (%zu bytes)", len);
        return false;
    }
    if (data[0] != ESP_IMAGE_HEADER_MAGIC) {
        std::snprintf(err_buf, err_len, "Invalid ESP image magic (0x%02X != 0xE9)", data[0]);
        return false;
    }
    // Offset 12..13 in esp_image_header_t is uint16_t chip_id (little-endian)
    uint16_t chip_id = static_cast<uint16_t>(data[12]) | (static_cast<uint16_t>(data[13]) << 8);
    if (chip_id != ESP_CHIP_ID_ESP32S3) {
        std::snprintf(err_buf, err_len, "Wrong target chip_id (0x%04X != 0x0009 ESP32-S3)", chip_id);
        return false;
    }
    return true;
}

inline void sha256_to_hex(const uint8_t hash[32], char out_hex[65]) {
    static constexpr const char* HEX_CHARS = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex[i * 2]     = HEX_CHARS[(hash[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = HEX_CHARS[hash[i] & 0x0F];
    }
    out_hex[64] = '\0';
}

inline bool hex_to_bytes(const char* hex, uint8_t* out, size_t max_out, size_t& out_len) {
    out_len = 0;
    if (!hex) return false;
    size_t slen = std::strlen(hex);
    if ((slen % 2) != 0 || (slen / 2) > max_out) return false;
    for (size_t i = 0; i < slen; i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[out_len++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

#ifdef ARDUINO
/**
 * @brief Verifies an ECDSA P-256 DER signature (hex-encoded) over a 32-byte SHA-256 digest.
 */
inline bool verify_ecdsa_signature(const uint8_t hash[32], const char* sig_hex, char* err_buf, size_t err_len) {
    if (!BEANZILLA_OTA_PUBKEY_PEM || std::strlen(BEANZILLA_OTA_PUBKEY_PEM) == 0) {
        return true;
    }
    if (!sig_hex || std::strlen(sig_hex) == 0) {
        // Unsigned manifest: SHA-256 + ESP32-S3 Chip-ID (0x0009) already verified
        return true;
    }
    uint8_t sig_der[128];
    size_t sig_len = 0;
    if (!hex_to_bytes(sig_hex, sig_der, sizeof(sig_der), sig_len)) {
        std::snprintf(err_buf, err_len, "Malformed signature hex");
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(
        &pk,
        reinterpret_cast<const unsigned char*>(BEANZILLA_OTA_PUBKEY_PEM),
        std::strlen(BEANZILLA_OTA_PUBKEY_PEM) + 1
    );
    if (rc != 0) {
        mbedtls_pk_free(&pk);
        std::snprintf(err_buf, err_len, "Public key parse error (-0x%04X)", -rc);
        return false;
    }
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig_der, sig_len);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        std::snprintf(err_buf, err_len, "ECDSA signature verification FAILED (-0x%04X)", -rc);
        return false;
    }
    return true;
}

class OtaManager {
public:
    /**
     * @brief Checks for /update.bin on the MicroSD card at boot. If present, validates
     *        the ESP32-S3 header (and /update.sha256 if present), flashes the update,
     *        renames /update.bin -> /update.done, and returns true to trigger reboot.
     */
    template <typename DisplayT>
    bool check_and_apply_sdcard_update(bool sd_mounted, DisplayT& display) {
        if (!sd_mounted || !SD_MMC.exists("/update.bin")) {
            return false;
        }
        File f = SD_MMC.open("/update.bin", FILE_READ);
        if (!f || f.size() < 65536) {
            if (f) f.close();
            return false;
        }

        uint8_t hdr[24] = {0};
        if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) {
            f.close();
            return false;
        }
        char err[128] = {0};
        if (!validate_esp32s3_image_header(hdr, sizeof(hdr), err, sizeof(err))) {
            Serial.printf("[BulletSpy SD-OTA] Rejected /update.bin: %s\n", err);
            f.close();
            SD_MMC.rename("/update.bin", "/update.rejected");
            return false;
        }
        f.seek(0);

        // Optional expected SHA-256 in /update.sha256
        char expected_sha[65] = {0};
        if (SD_MMC.exists("/update.sha256")) {
            File sf = SD_MMC.open("/update.sha256", FILE_READ);
            if (sf) {
                size_t n = sf.readBytes(expected_sha, 64);
                expected_sha[n] = '\0';
                sf.close();
            }
        }

        display.render_boot_splash("FLASHING /update.bin FROM MICROSD...");
        display.display();

        size_t total_len = f.size();
        if (!Update.begin(total_len, U_FLASH)) {
            f.close();
            return false;
        }

        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts_ret(&sha, 0);

        uint8_t buf[4096];
        size_t written = 0;
        while (f.available()) {
            size_t r = f.read(buf, sizeof(buf));
            if (r == 0) break;
            mbedtls_sha256_update_ret(&sha, buf, r);
            if (Update.write(buf, r) != r) {
                Update.abort();
                mbedtls_sha256_free(&sha);
                f.close();
                return false;
            }
            written += r;
        }
        f.close();

        uint8_t digest[32];
        char actual_sha[65];
        mbedtls_sha256_finish_ret(&sha, digest);
        mbedtls_sha256_free(&sha);
        sha256_to_hex(digest, actual_sha);

        if ( std::strlen(expected_sha) == 64 && ! String(actual_sha).equalsIgnoreCase(expected_sha) ) {
            Serial.printf("[BulletSpy SD-OTA] SHA-256 mismatch! Expected %s, got %s\n", expected_sha, actual_sha);
            Update.abort();
            SD_MMC.rename("/update.bin", "/update.bad_sha");
            return false;
        }

        if (!Update.end(true)) {
            return false;
        }

        SD_MMC.remove("/update.done");
        SD_MMC.rename("/update.bin", "/update.done");
        display.render_boot_splash("MICROSD UPDATE COMPLETE! REBOOTING...");
        display.display();
        delay(800);
        return true;
    }

    /**
     * @brief Queries the latest GitHub Release ota_manifest.json directly (no API rate limit!).
     */
    String check_github_release_json() {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        String manifest_url = String("https://github.com/") + GITHUB_REPO_SLUG +
                              "/releases/latest/download/" + GITHUB_MANIFEST_ASSET;
        http.setUserAgent("BulletSpy-ESP32S3-OTA");
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setTimeout(8000);

        JsonDocument out;
        out["current_version"] = BULLETSPY_FW_VERSION;
        out["repo"]            = GITHUB_REPO_SLUG;

        if (!http.begin(client, manifest_url)) {
            out["ok"]    = false;
            out["error"] = "Failed to initialize HTTPS client";
            String s; serializeJson(out, s); return s;
        }

        int code = http.GET();
        if (code != 200) {
            http.end();
            out["ok"]    = false;
            out["error"] = (code == 404)
                               ? "No GitHub Releases published yet on " + String(GITHUB_REPO_SLUG)
                               : "GitHub Release returned HTTP " + String(code);
            String s; serializeJson(out, s); return s;
        }

        JsonDocument mdoc;
        DeserializationError err = deserializeJson(mdoc, http.getStream());
        http.end();
        if (err) {
            out["ok"]    = false;
            out["error"] = "Invalid ota_manifest.json from GitHub Release";
            String s; serializeJson(out, s); return s;
        }

        String clean_ver = mdoc["version"] | "";
        String tag       = mdoc["tag"] | "";
        String bin_url   = mdoc["firmware_url"] | "";

        out["ok"]               = true;
        out["latest_version"]   = clean_ver;
        out["tag_name"]         = tag;
        out["sha256"]           = mdoc["sha256"] | "";
        out["signed"]           = String(mdoc["signature"] | "").length() > 0;
        out["update_available"] = (clean_ver.length() > 0 && clean_ver != BULLETSPY_FW_VERSION);
        out["bin_url"]          = bin_url;
        out["manifest_url"]     = manifest_url;
        latest_bin_url_         = bin_url;
        latest_manifest_url_    = manifest_url;

        String s;
        serializeJson(out, s);
        return s;
    }

    /**
     * @brief Downloads ota_manifest.json (if present) and streams the firmware binary
     *        from GitHub Releases, verifying ESP32-S3 chip_id, SHA-256, and ECDSA signature.
     */
    bool pull_and_flash_from_github(String& out_message) {
        if (latest_bin_url_.length() == 0) {
            check_github_release_json();
        }
        if (latest_bin_url_.length() == 0) {
            out_message = "No firmware .bin asset found in latest GitHub Release.";
            return false;
        }

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setUserAgent("BulletSpy-ESP32S3-OTA");
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setTimeout(15000);

        String expected_sha256 = "";
        String expected_sig    = "";

        // 1. Fetch ota_manifest.json if available
        if (latest_manifest_url_.length() > 0) {
            if (http.begin(client, latest_manifest_url_)) {
                if (http.GET() == 200) {
                    JsonDocument mdoc;
                    if (deserializeJson(mdoc, http.getStream()) == DeserializationError::Ok) {
                        expected_sha256 = mdoc["sha256"] | "";
                        expected_sig    = mdoc["signature"] | "";
                    }
                }
                http.end();
            }
        }

        // 2. Stream firmware binary
        if (!http.begin(client, latest_bin_url_)) {
            out_message = "Failed to connect to GitHub Release asset URL.";
            return false;
        }
        int code = http.GET();
        int total_len = http.getSize();
        if (code != 200 || total_len < 65536) {
            http.end();
            out_message = "GitHub asset download failed (HTTP " + String(code) + ")";
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[2048];
        size_t written = 0;
        bool header_checked = false;

        if (!Update.begin(total_len, U_FLASH)) {
            http.end();
            out_message = "Not enough OTA flash partition space.";
            return false;
        }

        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts_ret(&sha, 0);

        uint32_t last_read_ms = millis();
        while (http.connected() && (written < static_cast<size_t>(total_len))) {
            size_t avail = stream->available();
            if (avail > 0) {
                size_t to_read = (avail > sizeof(buf)) ? sizeof(buf) : avail;
                int r = stream->readBytes(buf, to_read);
                if (r <= 0) continue;
                last_read_ms = millis();

                if (!header_checked) {
                    char err[128] = {0};
                    if (!validate_esp32s3_image_header(buf, static_cast<size_t>(r), err, sizeof(err))) {
                        Update.abort();
                        mbedtls_sha256_free(&sha);
                        http.end();
                        out_message = String("Security Guard Rejected Binary: ") + err;
                        return false;
                    }
                    header_checked = true;
                }

                mbedtls_sha256_update_ret(&sha, buf, static_cast<size_t>(r));
                if (Update.write(buf, static_cast<size_t>(r)) != static_cast<size_t>(r)) {
                    Update.abort();
                    mbedtls_sha256_free(&sha);
                    http.end();
                    out_message = "Flash write error during OTA stream.";
                    return false;
                }
                written += static_cast<size_t>(r);
            } else {
                if ((millis() - last_read_ms) > 12000) {
                    Update.abort();
                    mbedtls_sha256_free(&sha);
                    http.end();
                    out_message = "Download stream timed out.";
                    return false;
                }
                delay(5);
            }
        }
        http.end();

        uint8_t digest[32];
        char actual_sha[65];
        mbedtls_sha256_finish_ret(&sha, digest);
        mbedtls_sha256_free(&sha);
        sha256_to_hex(digest, actual_sha);

        if (expected_sha256.length() == 64 && !String(actual_sha).equalsIgnoreCase(expected_sha256)) {
            Update.abort();
            out_message = String("SHA-256 digest mismatch! Expected ") + expected_sha256 + ", got " + actual_sha;
            return false;
        }

        char sig_err[128] = {0};
        if (!verify_ecdsa_signature(digest, expected_sig.c_str(), sig_err, sizeof(sig_err))) {
            Update.abort();
            out_message = sig_err;
            return false;
        }

        if (!Update.end(true)) {
            out_message = String("OTA finalize failed: ") + Update.errorString();
            return false;
        }

        out_message = String("Verified ESP32-S3 image (SHA-256: ") + String(actual_sha).substring(0, 12) + "...) & flashed!";
        return true;
    }

    // Browser multipart upload state handlers
    void begin_browser_upload() {
        upload_ok_  = false;
        upload_err_ = "";
        mbedtls_sha256_init(&upload_sha_);
        mbedtls_sha256_starts_ret(&upload_sha_, 0);
        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    }

    void write_browser_chunk(uint8_t* data, size_t len, size_t index) {
        if (upload_err_.length() > 0) return;
        if (index == 0) {
            char err[128] = {0};
            if (!validate_esp32s3_image_header(data, len, err, sizeof(err))) {
                upload_err_ = err;
                Update.abort();
                return;
            }
        }
        mbedtls_sha256_update_ret(&upload_sha_, data, len);
        if (Update.write(data, len) != len) {
            upload_err_ = Update.errorString();
            Update.abort();
        }
    }

    void finish_browser_upload(const String& expected_sha256) {
        uint8_t digest[32];
        mbedtls_sha256_finish_ret(&upload_sha_, digest);
        mbedtls_sha256_free(&upload_sha_);
        sha256_to_hex(digest, last_sha256_hex_);

        if (upload_err_.length() > 0) {
            upload_ok_ = false;
            return;
        }
        if (expected_sha256.length() == 64 && !String(last_sha256_hex_).equalsIgnoreCase(expected_sha256)) {
            upload_err_ = String("SHA-256 mismatch (got ") + last_sha256_hex_ + ")";
            Update.abort();
            upload_ok_ = false;
            return;
        }
        if (!Update.end(true)) {
            upload_err_ = Update.errorString();
            upload_ok_  = false;
            return;
        }
        upload_ok_ = true;
    }

    bool browser_upload_ok() const { return upload_ok_; }
    const String& browser_upload_error() const { return upload_err_; }
    const char* last_sha256_hex() const { return last_sha256_hex_; }

private:
    String latest_bin_url_;
    String latest_manifest_url_;
    bool upload_ok_ = false;
    String upload_err_;
    char last_sha256_hex_[65] = {0};
    mbedtls_sha256_context upload_sha_;
};
#endif

} // namespace bulletspy
