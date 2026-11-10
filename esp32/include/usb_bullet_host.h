#pragma once
/**
 * @file usb_bullet_host.h
 * @brief Native ESP32-S3 USB OTG Host Driver for Aillio Bullet R2 & R1/IBTS.
 *
 * SPDX-License-Identifier: PolyForm-Shield-1.0.0
 * Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
 * Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
 *
 * Uses the ESP-IDF USB Host Library (usb/usb_host.h) to claim Interface 0x01
 * on STM32 VID 0x0483 (PIDs 0xA4CD, 0xA27E, 0x5741) and exchange 64-byte bulk
 * frames on EP_IN (0x81) and EP_OUT (0x03) at 4Hz (250ms).
 *
 * Automatically falls back to simulated binary frame synthesis when unplugged,
 * and skips USB Host takeover if the side [KEY] button (GPIO18) is held at boot.
 */

#include <cstring>
#include <mutex>
#include <vector>

#include "bullet_protocol.h"
#include "pins_rlcd42.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/usb_host.h>
#endif

namespace bulletspy {

class BulletUsbHost {
public:
    void begin(bool allow_write = false, bool skip_usb_host = false) {
        state_.allow_write = allow_write;
        usb_host_enabled_  = !skip_usb_host;
#ifdef ARDUINO
        t0_ms_ = millis();
        if (usb_host_enabled_) {
            xTaskCreatePinnedToCore(
                usb_daemon_task_trampoline,
                "usb_daemon",
                4096,
                this,
                5,
                &daemon_task_,
                0
            );
            xTaskCreatePinnedToCore(
                usb_client_task_trampoline,
                "usb_client",
                6144,
                this,
                4,
                &client_task_,
                0
            );
        }
#endif
    }

    /**
     * @brief Called at 4Hz (every 250ms) to either poll live USB frames or
     *        synthesize realistic mock R2 binary frames when unplugged.
     */
    void poll_step(double now_sec) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.timestamp_sec = now_sec;

        if (!device_connected_) {
            state_.mode     = "SIMULATED";
            state_.model    = "Simulated Aillio Bullet R2";
            state_.protocol = 2;
            if (state_.cycle_id == 0x00 && state_.heater_power == 0) {
                state_.cycle_id     = 0x06;
                state_.heater_power = 7;
                state_.fan_speed    = 4;
                state_.drum_speed   = 9;
            }
            uint8_t a0[64], a1[64], a2[64];
            build_mock_r2_frames(now_sec, state_, a0, a1, a2);
            decode_r2_frame(a0, 64, state_);
            decode_r2_frame(a1, 64, state_);
            decode_r2_frame(a2, 64, state_);
        }
#ifdef ARDUINO
        else {
            // Live USB device attached: flush any queued write commands, then request status
            flush_command_queue_locked();
            request_live_telemetry_locked();
        }
#endif

        // Push to rolling chart history once per second (every 4th poll at 4Hz)
        if ((poll_counter_++ % 4) == 0) {
            state_.push_history(state_.bean_temp_c, state_.drum_temp_ibts_c, state_.bean_ror_c_min);
        }
    }

    bool dispatch_command(const std::string& action, int value, bool has_value,
                          std::vector<std::string>& out_hex_pkts) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (action == "target_drop" && has_value) {
            state_.target_drop_c = static_cast<float>(value);
            return true;
        }

        std::vector<std::vector<uint8_t>> pkts;
        if (!encode_command(action, value, has_value, state_, pkts)) {
            return false;
        }
        for (const auto& p : pkts) {
            char hex[130] = {0};
            bytes_to_hex(p.data(), p.size(), hex);
            out_hex_pkts.emplace_back(hex);
            cmd_queue_.push_back(p);
        }
        return true;
    }

    BulletState snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    void update_board_metrics(float room_temp_c, float room_rh_pct, float bat_v, uint8_t bat_pct) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.room_temp_c       = room_temp_c;
        state_.room_humidity_pct = room_rh_pct;
        state_.battery_v         = bat_v;
        state_.battery_pct       = bat_pct;
    }

    void set_allow_write(bool allow) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.allow_write = allow;
    }

    void set_target_drop(float target_c) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.target_drop_c = target_c;
    }

private:
    std::mutex mutex_;
    BulletState state_;
    bool usb_host_enabled_ = true;
    bool device_connected_ = false;
    uint32_t poll_counter_ = 0;
    std::vector<std::vector<uint8_t>> cmd_queue_;

#ifdef ARDUINO
    uint32_t t0_ms_ = 0;
    TaskHandle_t daemon_task_ = nullptr;
    TaskHandle_t client_task_ = nullptr;
    usb_host_client_handle_t client_hdl_ = nullptr;
    usb_device_handle_t dev_hdl_ = nullptr;
    usb_transfer_t* xfer_in_  = nullptr;
    usb_transfer_t* xfer_out_ = nullptr;
    uint8_t ep_in_addr_  = DEFAULT_EP_IN;
    uint8_t ep_out_addr_ = DEFAULT_EP_OUT;

    static void usb_daemon_task_trampoline(void* arg) {
        static_cast<BulletUsbHost*>(arg)->usb_daemon_loop();
    }

    static void usb_client_task_trampoline(void* arg) {
        static_cast<BulletUsbHost*>(arg)->usb_client_loop();
    }

    void usb_daemon_loop() {
        usb_host_config_t host_cfg = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        if (usb_host_install(&host_cfg) != ESP_OK) {
            vTaskDelete(nullptr);
            return;
        }
        while (true) {
            uint32_t event_flags = 0;
            usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        }
    }

    static void client_event_cb(const usb_host_client_event_msg_t* event_msg, void* arg) {
        auto* self = static_cast<BulletUsbHost*>(arg);
        if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
            self->on_device_attached(event_msg->new_dev.address);
        } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
            self->on_device_detached();
        }
    }

    static void in_transfer_cb(usb_transfer_t* xfer) {
        auto* self = static_cast<BulletUsbHost*>(xfer->context);
        if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes >= static_cast<int>(FRAME_SIZE)) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->state_.protocol == 2) {
                decode_r2_frame(xfer->data_buffer, FRAME_SIZE, self->state_);
            } else {
                decode_r1_packets(xfer->data_buffer, FRAME_SIZE, nullptr, 0, self->state_);
            }
            self->state_.mode = "LIVE_USB";
        }
    }

    static void out_transfer_cb(usb_transfer_t* /*xfer*/) {}

    void usb_client_loop() {
        vTaskDelay(pdMS_TO_TICKS(150));
        usb_host_client_config_t client_cfg = {
            .is_synchronous = false,
            .max_num_event_msg = 5,
            .async = {
                .client_event_callback = client_event_cb,
                .callback_arg = this,
            },
        };
        if (usb_host_client_register(&client_cfg, &client_hdl_) != ESP_OK) {
            vTaskDelete(nullptr);
            return;
        }
        usb_host_transfer_alloc(FRAME_SIZE, 0, &xfer_in_);
        usb_host_transfer_alloc(FRAME_SIZE, 0, &xfer_out_);

        while (true) {
            usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(50));
        }
    }

    void on_device_attached(uint8_t dev_addr) {
        if (usb_host_device_open(client_hdl_, dev_addr, &dev_hdl_) != ESP_OK) {
            return;
        }
        const usb_device_desc_t* desc = nullptr;
        if (usb_host_get_device_descriptor(dev_hdl_, &desc) != ESP_OK || !desc) {
            usb_host_device_close(client_hdl_, dev_hdl_);
            dev_hdl_ = nullptr;
            return;
        }

        if (desc->idVendor != AILLIO_VID) {
            usb_host_device_close(client_hdl_, dev_hdl_);
            dev_hdl_ = nullptr;
            return;
        }

        // Claim interface 1 (or fallback to 0)
        if (usb_host_interface_claim(client_hdl_, dev_hdl_, AILLIO_INTERFACE, 0) != ESP_OK) {
            usb_host_interface_claim(client_hdl_, dev_hdl_, 0, 0);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        device_connected_ = true;
        state_.mode = "LIVE_USB";
        if (desc->idProduct == PID_BULLET_R2) {
            state_.protocol = 2;
            state_.model    = "Aillio Bullet R2";
        } else if (desc->idProduct == PID_BULLET_R1_IBTS) {
            state_.protocol = 1;
            state_.model    = "Aillio Bullet R1 IBTS";
        } else {
            state_.protocol = 1;
            state_.model    = "Aillio Bullet R1";
        }
    }

    void on_device_detached() {
        std::lock_guard<std::mutex> lock(mutex_);
        device_connected_ = false;
        if (dev_hdl_) {
            usb_host_interface_release(client_hdl_, dev_hdl_, AILLIO_INTERFACE);
            usb_host_device_close(client_hdl_, dev_hdl_);
            dev_hdl_ = nullptr;
        }
    }

    void flush_command_queue_locked() {
        if (!dev_hdl_ || !xfer_out_ || !state_.allow_write || cmd_queue_.empty()) {
            cmd_queue_.clear();
            return;
        }
        const auto pkt = cmd_queue_.front();
        cmd_queue_.erase(cmd_queue_.begin());
        xfer_out_->device_handle    = dev_hdl_;
        xfer_out_->bEndpointAddress = ep_out_addr_;
        xfer_out_->callback         = out_transfer_cb;
        xfer_out_->context          = this;
        xfer_out_->num_bytes        = pkt.size();
        std::memcpy(xfer_out_->data_buffer, pkt.data(), pkt.size());
        usb_host_transfer_submit(xfer_out_);
    }

    void request_live_telemetry_locked() {
        if (!dev_hdl_ || !xfer_in_) return;

        // Submit 64-byte Bulk IN read on 0x81
        xfer_in_->device_handle    = dev_hdl_;
        xfer_in_->bEndpointAddress = ep_in_addr_;
        xfer_in_->callback         = in_transfer_cb;
        xfer_in_->context          = this;
        xfer_in_->num_bytes        = FRAME_SIZE;
        usb_host_transfer_submit(xfer_in_);

        // For R1 protocol, explicitly prompt with CMD_STATUS1
        if (state_.protocol == 1 && xfer_out_ && cmd_queue_.empty()) {
            xfer_out_->device_handle    = dev_hdl_;
            xfer_out_->bEndpointAddress = ep_out_addr_;
            xfer_out_->callback         = out_transfer_cb;
            xfer_out_->context          = this;
            xfer_out_->num_bytes        = sizeof(CMD_STATUS1);
            std::memcpy(xfer_out_->data_buffer, CMD_STATUS1, sizeof(CMD_STATUS1));
            usb_host_transfer_submit(xfer_out_);
        }
    }
#endif
};

} // namespace bulletspy
