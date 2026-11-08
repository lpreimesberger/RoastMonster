#!/usr/bin/env python3
"""
BulletSpy (v0.1 POC)
Asynchronous 4Hz USB Telemetry Poller, Binary Frame Unpacker, FastAPI WebSocket
Broadcaster, and Dark-Mode Dashboard for the Aillio Bullet R1 / R2.

By default, BulletSpy runs in safe READ-ONLY mode (only polling telemetry).
Set BULLETSPY_ALLOW_WRITE=1 (or pass --allow-write) to enable command dispatch.
"""

from __future__ import annotations

import argparse
import asyncio
from contextlib import asynccontextmanager
import json
import logging
import math
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import time
from typing import Any, Dict, List, Optional, Set

import usb.core  # type: ignore[import-untyped]
import usb.util  # type: ignore[import-untyped]
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from pydantic import BaseModel

# --- LOGGING ---
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("bulletspy")

# --- CONFIGURATION ---
AILLIO_VID = 0x0483
KNOWN_DEVICES = [
    {"vid": 0x0483, "pid": 0xA4CD, "protocol": 2, "model": "Aillio Bullet R2"},
    {"vid": 0x0483, "pid": 0xA27E, "protocol": 1, "model": "Aillio Bullet R1 IBTS (Rev3)"},
    {"vid": 0x0483, "pid": 0x5741, "protocol": 1, "model": "Aillio Bullet R1"},
]

AILLIO_CONFIGURATION = 0x01
AILLIO_INTERFACE = 0x01
DEFAULT_EP_OUT = 0x03
DEFAULT_EP_IN = 0x81
FRAME_SIZE = 64
POLL_INTERVAL = float(os.environ.get("BULLETSPY_POLL_INTERVAL", "0.25"))  # 4 Hz
ALLOW_WRITE = os.environ.get("BULLETSPY_ALLOW_WRITE", "0").lower() in ("1", "true", "yes")

# Opcodes (shared across R1 & R2; R2 appends 4-byte STM32 CRC-32)
CMD_STATUS1 = [0x30, 0x01]
CMD_STATUS2 = [0x30, 0x03]
CMD_INFO1 = [0x30, 0x02]
CMD_INFO2 = [0x89, 0x01]
CMD_PRS = [0x30, 0x01, 0x00, 0x00]
CMD_HEATER_INCR = [0x34, 0x01, 0xAA, 0xAA]
CMD_HEATER_DECR = [0x34, 0x02, 0xAA, 0xAA]
CMD_DRUM_INCR = [0x32, 0x01, 0xAA, 0xAA]
CMD_DRUM_DECR = [0x32, 0x02, 0xAA, 0xAA]
CMD_FAN_INCR = [0x31, 0x01, 0xAA, 0xAA]
CMD_FAN_DECR = [0x31, 0x02, 0xAA, 0xAA]

STATE_MAP: Dict[int, str] = {
    0x00: "OFF",
    0x02: "PREHEATING",
    0x03: "STABILIZING",
    0x04: "CHARGE",
    0x06: "ROASTING",
    0x07: "COOLDOWN",
    0x08: "COOLING",
    0x09: "SHUTDOWN",
    0x0B: "POWER_ON_RESET",
}


def compute_roast_stage(bean_temp_c: float, cycle_id: int = 0x06) -> Dict[str, Any]:
    """
    Maps bean temperature (°C) and roaster cycle state into a human-readable
    roast level / chemical phase label, description, color, and progress percentage.
    """
    progress_pct = round(max(0.0, min(100.0, ((bean_temp_c - 90.0) / (232.0 - 90.0)) * 100.0)), 1)

    if cycle_id == 0x00:
        return {
            "roast_stage": "IDLE / STANDBY",
            "roast_stage_short": "STANDBY",
            "roast_stage_desc": "Roaster idle — waiting for preheat",
            "roast_stage_color": "#6b7280",
            "roast_progress_pct": 0.0,
        }
    if cycle_id in (0x02, 0x03):
        return {
            "roast_stage": "PREHEATING DRUM",
            "roast_stage_short": "PREHEAT",
            "roast_stage_desc": "Thermal soak — bringing drum to charge setpoint",
            "roast_stage_color": "#f59e0b",
            "roast_progress_pct": 0.0,
        }
    if cycle_id == 0x04:
        return {
            "roast_stage": "PREHEAT COMPLETE // CHARGE",
            "roast_stage_short": "CHARGE READY",
            "roast_stage_desc": "Preheat complete — ready to drop green beans!",
            "roast_stage_color": "#10b981",
            "roast_progress_pct": 0.0,
        }
    if cycle_id in (0x07, 0x08):
        return {
            "roast_stage": "COOLING BEANS",
            "roast_stage_short": "COOLING",
            "roast_stage_desc": "Roast dropped — cooling tray active",
            "roast_stage_color": "#3b82f6",
            "roast_progress_pct": 100.0,
        }

    # Active roast stages by bean temperature (°C)
    if bean_temp_c < 150.0:
        return {
            "roast_stage": "DRYING PHASE",
            "roast_stage_short": "DRYING",
            "roast_stage_desc": "Endothermic moisture evaporation (<150°C)",
            "roast_stage_color": "#10b981",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 175.0:
        return {
            "roast_stage": "YELLOWING / MAILLARD",
            "roast_stage_short": "MAILLARD",
            "roast_stage_desc": "Browning reactions & bread/hay aromas (150–175°C)",
            "roast_stage_color": "#eab308",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 196.0:
        return {
            "roast_stage": "CARAMELIZATION (PRE-FC)",
            "roast_stage_short": "PRE-1ST CRACK",
            "roast_stage_desc": "Sucrose caramelization, approaching First Crack (175–196°C)",
            "roast_stage_color": "#f59e0b",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 205.0:
        return {
            "roast_stage": "FIRST CRACK (CINNAMON / LIGHT)",
            "roast_stage_short": "FIRST CRACK",
            "roast_stage_desc": "Exothermic steam pop — Light / New England roast (196–205°C)",
            "roast_stage_color": "#f97316",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 214.0:
        return {
            "roast_stage": "CITY ROAST (MEDIUM-LIGHT)",
            "roast_stage_short": "CITY",
            "roast_stage_desc": "1st Crack clearing — bright, balanced origin character (205–214°C)",
            "roast_stage_color": "#ea580c",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 220.0:
        return {
            "roast_stage": "CITY+ / FULL CITY (MEDIUM)",
            "roast_stage_short": "FULL CITY",
            "roast_stage_desc": "Edge of 2nd Crack — rich body & chocolate/caramel (214–220°C)",
            "roast_stage_color": "#ef4444",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 226.0:
        return {
            "roast_stage": "FULL CITY+ / VIENNA (MED-DARK)",
            "roast_stage_short": "VIENNA",
            "roast_stage_desc": "Rolling 2nd Crack — surface oil droplets (220–226°C)",
            "roast_stage_color": "#dc2626",
            "roast_progress_pct": progress_pct,
        }
    if bean_temp_c < 232.0:
        return {
            "roast_stage": "FRENCH ROAST (DARK)",
            "roast_stage_short": "FRENCH",
            "roast_stage_desc": "Heavy roast character, smoky sugars & glossy oils (226–232°C)",
            "roast_stage_color": "#a855f7",
            "roast_progress_pct": progress_pct,
        }
    return {
        "roast_stage": "ITALIAN / DARK+ (DROP NOW!)",
        "roast_stage_short": "ITALIAN / DROP!",
        "roast_stage_desc": "High carbonization risk (>232°C) — Drop immediately!",
        "roast_stage_color": "#ec4899",
        "roast_progress_pct": 100.0,
    }


def calculate_stm32_crc32(data: bytes) -> int:
    """
    STM32 hardware CRC-32 used by Aillio Bullet R2 command packets.
    Matches Artisan's AillioR2.calculate_crc32 implementation.
    """
    short_lookup = [
        0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
        0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
        0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61,
        0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
    ]

    def crc32_fast(arg1: int, arg2: int) -> int:
        rax = (arg1 ^ arg2) & 0xFFFFFFFF
        for _ in range(4):
            idx = (rax >> 0x1C) & 0xF
            rax = ((rax << 4) & 0xFFFFFFFF) ^ short_lookup[idx]
        return rax & 0xFFFFFFFF

    data_copy = bytearray(data)
    data_copy[-4:] = [0, 0, 0, 0]

    result = 0xFFFFFFFF
    for i in range(0, len(data_copy), 4):
        val = int.from_bytes(data_copy[i : i + 4], "big")
        result = crc32_fast(result, val)

    return result & 0xFFFFFFFF


def prepare_r2_command(cmd: List[int] | bytes) -> bytes:
    """Appends the 4-byte little-endian STM32 CRC-32 required by Bullet R2."""
    raw = bytearray(bytes(cmd))
    raw.extend([0, 0, 0, 0])
    crc = calculate_stm32_crc32(bytes(raw))
    raw[-4:] = crc.to_bytes(4, "little")
    return bytes(raw)


def _find_linux_libusb_backend() -> Any:
    """Finds system libusb-1.0 shared library on x86_64 or Raspberry Pi (aarch64/armhf)."""
    if not platform.system().startswith("Linux"):
        return None
    for candidate in (
        "/usr/lib/aarch64-linux-gnu/libusb-1.0.so.0",
        "/usr/lib/aarch64-linux-gnu/libusb-1.0.so",
        "/usr/lib/arm-linux-gnueabihf/libusb-1.0.so.0",
        "/usr/lib/x86_64-linux-gnu/libusb-1.0.so.0",
        "/usr/lib/x86_64-linux-gnu/libusb-1.0.so",
    ):
        if os.path.isfile(candidate):
            try:
                from usb.backend import libusb1  # type: ignore[import-untyped]
                return libusb1.get_backend(find_library=lambda _, p=candidate: p)
            except Exception:
                pass
    return None


class BulletDriver:
    """
    Unified USB driver for Aillio Bullet R2 (protocol v2) and R1 (protocol v1),
    with a high-fidelity simulated binary frame generator when unplugged.
    """

    def __init__(self, allow_write: bool = False) -> None:
        self.dev: Any = None
        self.ep_out_addr: int = DEFAULT_EP_OUT
        self.ep_in_addr: int = DEFAULT_EP_IN
        self.interface_num: int = AILLIO_INTERFACE
        self.protocol: int = 2
        self.model: str = "Simulated Aillio Bullet R2"
        self.mock_mode: bool = True
        self.allow_write: bool = allow_write
        self.t0: float = time.time()
        self._cmd_queue: List[bytes] = []

        # Cached state across multiplexed R2 frames (0xA0, 0xA1, 0xA2)
        self.state: Dict[str, Any] = self._default_state()

    def _default_state(self) -> Dict[str, Any]:
        return {
            "timestamp": round(time.time(), 3),
            "mode": "SIMULATED",
            "model": self.model,
            "protocol": self.protocol,
            "allow_write": self.allow_write,
            "cycle_id": 0x00,
            "cycle": "OFF",
            "roast_clock": "00:00",
            "minutes": 0,
            "seconds": 0,
            # Core temperatures & RoR
            "drum_temp_ibts_c": 0.0,
            "drum_ror_c_min": 0.0,
            "bean_temp_c": 0.0,
            "bean_ror_c_min": 0.0,
            "exhaust_temp_c": 0.0,
            "inlet_temp_c": 0.0,
            "hot_air_temp_c": 0.0,
            "ibts_ambient_c": 0.0,
            # Setpoints & mechanical speeds
            "heater_power": 0,
            "fan_speed": 1,
            "drum_speed": 1,
            "fan_rpm": 0,
            "drum_rpm": 0,
            "ibts_fan_rpm": 0,
            "induction_fan_rpm": 0,
            # Electrical / R2 power metrics
            "voltage_v": 0.0,
            "current_a": 0.0,
            "active_power_w": 0.0,
            "power_setpoint_w": 0,
            "line_freq_hz": 0.0,
            "igbt_temp1_c": 0.0,
            "igbt_temp2_c": 0.0,
            "energy_used_wh": 0.0,
            # Environment & events
            "diff_pressure_pa": 0.0,
            "pressure_hpa": 0.0,
            "humidity_pct": 0.0,
            "crack_count": 0,
            "error_counts": 0,
            # Roast stage classification
            "roast_stage": "IDLE / STANDBY",
            "roast_stage_short": "STANDBY",
            "roast_stage_desc": "Roaster idle — waiting for preheat",
            "roast_stage_color": "#6b7280",
            "roast_progress_pct": 0.0,
            # Raw packet hex inspector
            "raw_pkt1_hex": "00" * FRAME_SIZE,
            "raw_pkt2_hex": "00" * FRAME_SIZE,
            "raw_frames": {
                "a0_temp": "",
                "a1_fan": "",
                "a2_power": "",
            },
        }

    def connect(self) -> None:
        self.close()
        backend = _find_linux_libusb_backend()
        found_variant = None

        try:
            for variant in KNOWN_DEVICES:
                dev = usb.core.find(
                    idVendor=variant["vid"],
                    idProduct=variant["pid"],
                    backend=backend,
                )
                if dev is not None:
                    self.dev = dev
                    found_variant = variant
                    break

            if self.dev is None:
                # Fallback: any STM32 device with Aillio VID
                dev = usb.core.find(idVendor=AILLIO_VID, backend=backend)
                if dev is not None:
                    self.dev = dev
                    pid = getattr(dev, "idProduct", 0)
                    proto = 2 if pid == 0xA4CD else 1
                    found_variant = {
                        "vid": AILLIO_VID,
                        "pid": pid,
                        "protocol": proto,
                        "model": f"Aillio Bullet (PID 0x{pid:04x})",
                    }
        except Exception as exc:
            log.warning("USB discovery error (%s); falling back to mock mode.", exc)
            self.dev = None

        if self.dev is None or found_variant is None:
            log.warning("No Aillio Bullet detected on USB. Running in SIMULATED MOCK MODE.")
            self.mock_mode = True
            self.protocol = 2
            self.model = "Simulated Aillio Bullet R2"
            self.state["mode"] = "SIMULATED"
            self.state["model"] = self.model
            self.state["protocol"] = self.protocol
            return

        self.protocol = int(found_variant["protocol"])
        self.model = str(found_variant["model"])
        log.info(
            "Found %s (VID=0x%04x PID=0x%04x, Protocol V%d)",
            self.model,
            self.dev.idVendor,
            self.dev.idProduct,
            self.protocol,
        )

        try:
            # Detach kernel driver on interface 1 (or 0) if active
            for intf_idx in (AILLIO_INTERFACE, 0):
                try:
                    if self.dev.is_kernel_driver_active(intf_idx):
                        self.dev.detach_kernel_driver(intf_idx)
                except Exception:
                    pass

            try:
                self.dev.set_configuration(AILLIO_CONFIGURATION)
            except Exception:
                pass

            cfg = self.dev.get_active_configuration()
            # Prefer interface 1 (used by Artisan R1 & R2), fallback to 0
            intf = None
            for candidate_intf in (AILLIO_INTERFACE, 0):
                try:
                    intf = cfg[(candidate_intf, 0)]
                    self.interface_num = candidate_intf
                    break
                except Exception:
                    continue

            if intf is not None:
                usb.util.claim_interface(self.dev, self.interface_num)
                for ep in intf:
                    direction = usb.util.endpoint_direction(ep.bEndpointAddress)
                    if direction == usb.util.ENDPOINT_OUT:
                        self.ep_out_addr = ep.bEndpointAddress
                    elif direction == usb.util.ENDPOINT_IN:
                        self.ep_in_addr = ep.bEndpointAddress
            else:
                self.interface_num = 0
                usb.util.claim_interface(self.dev, 0)

            log.info(
                "Claimed USB interface %d (EP_OUT=0x%02x, EP_IN=0x%02x)",
                self.interface_num,
                self.ep_out_addr,
                self.ep_in_addr,
            )
            self.mock_mode = False
            self.state["mode"] = "LIVE_USB"
            self.state["model"] = self.model
            self.state["protocol"] = self.protocol
        except Exception as exc:
            log.error("Failed to claim Bullet USB interface (%s). Using SIMULATED mode.", exc)
            self.dev = None
            self.mock_mode = True
            self.state["mode"] = "SIMULATED"

    def close(self) -> None:
        if self.dev is not None:
            try:
                usb.util.release_interface(self.dev, self.interface_num)
                usb.util.dispose_resources(self.dev)
            except Exception:
                pass
            self.dev = None

    def poll(self) -> Dict[str, Any]:
        self.state["allow_write"] = self.allow_write
        if self.mock_mode:
            return self._poll_mock()

        try:
            # Flush any queued control commands if write mode is enabled
            while self._cmd_queue:
                cmd_bytes = self._cmd_queue.pop(0)
                if self.allow_write:
                    self.dev.write(self.ep_out_addr, cmd_bytes, timeout=300)

            if self.protocol == 2:
                return self._poll_r2_usb()
            return self._poll_r1_usb()
        except Exception as exc:
            log.error("USB Read Error: %s. Attempting reconnect...", exc)
            self.connect()
            return self._poll_mock()

    def _poll_r2_usb(self) -> Dict[str, Any]:
        """
        Reads up to 3 back-to-back 64-byte frames (0xA0 Temp, 0xA1 Fan, 0xA2 Power)
        from the R2 IN endpoint. If the endpoint times out waiting for unsolicited frames,
        sends a benign status inquiry heartbeat.
        """
        frames_read: List[bytes] = []
        for _ in range(3):
            try:
                raw = bytes(self.dev.read(self.ep_in_addr, FRAME_SIZE, timeout=120))
                if len(raw) == FRAME_SIZE:
                    frames_read.append(raw)
                    self.decode_r2_frame(raw)
            except usb.core.USBTimeoutError:
                break

        if not frames_read:
            # Prompt R2 if silent
            self.dev.write(self.ep_out_addr, bytes(CMD_STATUS1), timeout=200)
            raw = bytes(self.dev.read(self.ep_in_addr, FRAME_SIZE, timeout=250))
            if len(raw) == FRAME_SIZE:
                frames_read.append(raw)
                self.decode_r2_frame(raw)

        if len(frames_read) >= 1:
            self.state["raw_pkt1_hex"] = frames_read[0].hex()
        if len(frames_read) >= 2:
            self.state["raw_pkt2_hex"] = frames_read[1].hex()

        self.state["timestamp"] = round(time.time(), 3)
        self.state["mode"] = "LIVE_USB"
        return dict(self.state)

    def _poll_r1_usb(self) -> Dict[str, Any]:
        """Polls Classic Aillio R1 / IBTS via two 64-byte status packets."""
        self.dev.write(self.ep_out_addr, bytes(CMD_STATUS1), timeout=200)
        pkt1 = bytes(self.dev.read(self.ep_in_addr, FRAME_SIZE, timeout=200))
        self.dev.write(self.ep_out_addr, bytes(CMD_STATUS2), timeout=200)
        pkt2 = bytes(self.dev.read(self.ep_in_addr, FRAME_SIZE, timeout=200))
        return self.decode_r1_packets(pkt1, pkt2, mode="LIVE_USB")

    def decode_r2_frame(self, data: bytes) -> Dict[str, Any]:
        """
        Unpacks a 64-byte Aillio Bullet R2 frame according to its header byte:
          - 0xA0: Temperature, RoR, air pressure, clock, setpoints, state machine
          - 0xA1: Fan duties, RPMs (coil, induction, IBTS, drum), error counts
          - 0xA2: Power setpoint (W), RMS voltage/current, active power, IGBT temps
        Matches Artisan's artisanlib/aillio_r2.py byte offsets.
        """
        if len(data) < FRAME_SIZE:
            return self.state

        frame_type = data[0]
        if frame_type == 0xA0:
            ibts_temp = round(struct.unpack("<f", data[4:8])[0], 1)
            ibts_ror = round(struct.unpack("<f", data[8:12])[0], 1)
            ibts_ambient = round(struct.unpack("<f", data[12:16])[0], 1)
            bt_probe = round(struct.unpack("<f", data[16:20])[0], 1)
            bt_ror = round(struct.unpack("<f", data[20:24])[0], 1)
            energy_wh = round(struct.unpack("<f", data[24:28])[0], 1)
            diff_pressure = round(struct.unpack("<f", data[28:32])[0], 2)
            exhaust_rpm = struct.unpack("<H", data[32:34])[0]
            inlet_temp = round(struct.unpack("<H", data[34:36])[0] / 10.0, 1)
            hot_air_temp = round(struct.unpack("<H", data[36:38])[0] / 10.0, 1)
            exhaust_temp = round(struct.unpack("<H", data[38:40])[0] / 10.0, 1)
            abs_pressure = round(struct.unpack("<H", data[40:42])[0] / 10.0, 1)
            humidity = round(struct.unpack("<H", data[42:44])[0] / 10.0, 1)

            minutes = data[46]
            seconds = data[48]
            heater = data[50]
            fan = data[51]
            drum = data[53]
            cracks = data[56]
            state_byte = data[59]

            self.state.update(
                {
                    "cycle_id": state_byte,
                    "cycle": STATE_MAP.get(state_byte, f"UNKNOWN (0x{state_byte:02x})"),
                    "minutes": minutes,
                    "seconds": seconds,
                    "roast_clock": f"{minutes:02d}:{seconds:02d}",
                    "drum_temp_ibts_c": ibts_temp,
                    "drum_ror_c_min": ibts_ror,
                    "ibts_ambient_c": ibts_ambient,
                    "bean_temp_c": bt_probe,
                    "bean_ror_c_min": bt_ror,
                    "energy_used_wh": energy_wh,
                    "diff_pressure_pa": diff_pressure,
                    "fan_rpm": exhaust_rpm,
                    "inlet_temp_c": inlet_temp,
                    "hot_air_temp_c": hot_air_temp,
                    "exhaust_temp_c": exhaust_temp,
                    "pressure_hpa": abs_pressure,
                    "humidity_pct": humidity,
                    "heater_power": heater,
                    "fan_speed": fan,
                    "drum_speed": drum,
                    "crack_count": cracks,
                    **compute_roast_stage(bt_probe, state_byte),
                }
            )
            self.state["raw_frames"]["a0_temp"] = data.hex()

        elif frame_type == 0xA1:
            error_counts = data[4]
            induction_fan1_rpm = struct.unpack("<H", data[38:40])[0]
            ibts_fan_rpm = struct.unpack("<H", data[46:48])[0]
            roast_drum_rpm = struct.unpack("<H", data[48:50])[0]

            self.state.update(
                {
                    "error_counts": error_counts,
                    "induction_fan_rpm": induction_fan1_rpm,
                    "ibts_fan_rpm": ibts_fan_rpm,
                    "drum_rpm": roast_drum_rpm,
                }
            )
            self.state["raw_frames"]["a1_fan"] = data.hex()

        elif frame_type == 0xA2:
            power_setpoint_w = struct.unpack("<H", data[4:6])[0]
            line_freq_hz = round(struct.unpack("<H", data[6:8])[0] / 100.0, 2)
            igbt_temp1 = round(struct.unpack("<H", data[12:14])[0] / 10.0, 1)
            igbt_temp2 = round(struct.unpack("<H", data[14:16])[0] / 10.0, 1)
            active_power_w = round(struct.unpack("<H", data[22:24])[0] / 10.0, 1)
            voltage_rms = round(struct.unpack("<H", data[28:30])[0] / 10.0, 1)
            current_rms = round(struct.unpack("<H", data[30:32])[0] / 1000.0, 2)

            self.state.update(
                {
                    "power_setpoint_w": power_setpoint_w,
                    "line_freq_hz": line_freq_hz,
                    "igbt_temp1_c": igbt_temp1,
                    "igbt_temp2_c": igbt_temp2,
                    "active_power_w": active_power_w,
                    "voltage_v": voltage_rms,
                    "current_a": current_rms,
                }
            )
            self.state["raw_frames"]["a2_power"] = data.hex()

        return self.state

    def decode_r1_packets(
        self, pkt1: bytes, pkt2: bytes, mode: str = "LIVE_USB"
    ) -> Dict[str, Any]:
        """
        Unpacks two 64-byte classic Aillio R1 / IBTS packets.
        Matches Artisan's artisanlib/aillio_r1.py offsets (with fallback if valid byte != 10).
        """
        if len(pkt1) >= FRAME_SIZE:
            # In Artisan's aillio_r1.py:
            # 0:4 = bt (IBTS), 4:8 = bt_ror, 8:12 = dt (Bean Probe), 16:20 = exitt
            # 24 = minutes, 25 = seconds, 26 = fan, 27 = heater, 28 = drum, 29 = r1state
            # 32:36 = irt, 36:40 = pcbt, 44:46 = fan_rpm, 48:50 = voltage
            dt_ibts = round(struct.unpack("<f", pkt1[0:4])[0], 1)
            dt_ror = round(struct.unpack("<f", pkt1[4:8])[0], 1)
            bt_probe = round(struct.unpack("<f", pkt1[8:12])[0], 1)
            bt_ror = round(struct.unpack("<f", pkt1[12:16])[0], 1)
            exhaust_temp = round(struct.unpack("<f", pkt1[16:20])[0], 1)

            minutes = pkt1[24]
            seconds = pkt1[25]
            fan_lvl = pkt1[26]
            heater_lvl = pkt1[27]
            drum_lvl = pkt1[28]
            state_byte = pkt1[29]
            fan_rpm = struct.unpack("<h", pkt1[44:46])[0]
            voltage = float(struct.unpack("<h", pkt1[48:50])[0])

            self.state.update(
                {
                    "timestamp": round(time.time(), 3),
                    "mode": mode,
                    "protocol": 1,
                    "cycle_id": state_byte,
                    "cycle": STATE_MAP.get(state_byte, f"UNKNOWN (0x{state_byte:02x})"),
                    "minutes": minutes,
                    "seconds": seconds,
                    "roast_clock": f"{minutes:02d}:{seconds:02d}",
                    "drum_temp_ibts_c": dt_ibts,
                    "drum_ror_c_min": dt_ror,
                    "bean_temp_c": bt_probe,
                    "bean_ror_c_min": bt_ror,
                    "exhaust_temp_c": exhaust_temp,
                    "heater_power": heater_lvl,
                    "fan_speed": fan_lvl,
                    "drum_speed": drum_lvl,
                    "fan_rpm": fan_rpm,
                    "voltage_v": round(voltage, 1),
                    "raw_pkt1_hex": pkt1.hex(),
                    "raw_pkt2_hex": pkt2.hex(),
                    **compute_roast_stage(bt_probe, state_byte),
                }
            )
        return dict(self.state)

    def _build_mock_r2_frames(self) -> tuple[bytes, bytes, bytes]:
        """
        Builds synthetic 64-byte 0xA0, 0xA1, and 0xA2 binary frames with STM32 CRC-32
        so Mock Mode exercises the exact same struct.unpack parser as live R2 hardware.
        """
        elapsed = time.time() - self.t0
        # Simulate a realistic roast curve sweeping from 140C (Drying) to 224C (Full City+)
        roast_sec = int(elapsed) % 600
        minutes = roast_sec // 60
        seconds = roast_sec % 60

        bt = round(182.0 + 42.0 * math.sin(elapsed / 35.0), 1)
        bt_ror = round((42.0 / 35.0) * 60.0 * math.cos(elapsed / 35.0) * 0.2, 1)
        ibts = round(bt + 21.5 + 3.5 * math.cos(elapsed / 12.0), 1)
        ibts_ror = round(bt_ror + 1.4 * math.sin(elapsed / 15.0), 1)
        exhaust = round(bt - 42.0, 1)
        inlet = round(28.5 + 1.2 * math.sin(elapsed / 40.0), 1)
        hot_air = round(ibts + 45.0, 1)
        energy_wh = round((roast_sec / 600.0) * 420.0, 1)
        diff_pressure = round(18.5 + 2.1 * math.sin(elapsed / 8.0), 2)

        heater = int(self.state.get("heater_power") or 7)
        fan = int(self.state.get("fan_speed") or 4)
        drum = int(self.state.get("drum_speed") or 9)
        cycle_id = int(self.state.get("cycle_id") or 0x06)

        # Frame 0xA0 (64 bytes)
        a0 = bytearray(FRAME_SIZE)
        a0[0] = 0xA0
        struct.pack_into("<f", a0, 4, ibts)
        struct.pack_into("<f", a0, 8, ibts_ror)
        struct.pack_into("<f", a0, 12, 34.2)  # IBTS ambient
        struct.pack_into("<f", a0, 16, bt)
        struct.pack_into("<f", a0, 20, bt_ror)
        struct.pack_into("<f", a0, 24, energy_wh)
        struct.pack_into("<f", a0, 28, diff_pressure)
        struct.pack_into("<H", a0, 32, 1420 + fan * 110)  # exhaust_fan_rpm
        struct.pack_into("<H", a0, 34, int(inlet * 10))
        struct.pack_into("<H", a0, 36, int(hot_air * 10))
        struct.pack_into("<H", a0, 38, int(exhaust * 10))
        struct.pack_into("<H", a0, 40, 10132)  # 1013.2 hPa
        struct.pack_into("<H", a0, 42, 445)  # 44.5 % RH
        struct.pack_into("<H", a0, 44, 265)  # 26.5 C
        a0[46] = minutes
        a0[48] = seconds
        a0[50] = heater
        a0[51] = fan
        a0[53] = drum
        a0[56] = 12 if bt > 196.0 else 0
        a0[59] = cycle_id
        struct.pack_into("<I", a0, 60, calculate_stm32_crc32(bytes(a0)))

        # Frame 0xA1 (64 bytes)
        a1 = bytearray(FRAME_SIZE)
        a1[0] = 0xA1
        a1[4] = 0  # error_counts
        struct.pack_into("<H", a1, 38, 2850)  # induction_fan1_rpm
        struct.pack_into("<H", a1, 46, 4100)  # ibts_fan_rpm
        struct.pack_into("<H", a1, 48, 52 + drum * 6)  # roast_drum_rpm
        struct.pack_into("<I", a1, 60, calculate_stm32_crc32(bytes(a1)))

        # Frame 0xA2 (64 bytes)
        a2 = bytearray(FRAME_SIZE)
        a2[0] = 0xA2
        watts = heater * 220
        volts = round(238.4 + 0.8 * math.sin(elapsed / 5.0), 1)
        amps = round(watts / volts, 2) if volts > 0 else 0.0
        struct.pack_into("<H", a2, 4, watts)  # power_setpoint_watt
        struct.pack_into("<H", a2, 6, 6000)  # 60.00 Hz
        struct.pack_into("<H", a2, 8, 24800)  # igbt_frequency_hz
        struct.pack_into("<H", a2, 12, 462)  # igbt_temp1 = 46.2 C
        struct.pack_into("<H", a2, 14, 448)  # igbt_temp2 = 44.8 C
        struct.pack_into("<H", a2, 22, int(watts * 10))  # active_power * 10
        struct.pack_into("<H", a2, 28, int(volts * 10))  # voltage_rms * 10
        struct.pack_into("<H", a2, 30, int(amps * 1000))  # current_rms * 1000
        struct.pack_into("<I", a2, 60, calculate_stm32_crc32(bytes(a2)))

        return bytes(a0), bytes(a1), bytes(a2)

    def _poll_mock(self) -> Dict[str, Any]:
        # Initialize defaults on first mock poll if zero
        if self.state["cycle_id"] == 0x00 and self.state["heater_power"] == 0:
            self.state["cycle_id"] = 0x06
            self.state["heater_power"] = 7
            self.state["fan_speed"] = 4
            self.state["drum_speed"] = 9

        a0, a1, a2 = self._build_mock_r2_frames()
        self.decode_r2_frame(a0)
        self.decode_r2_frame(a1)
        self.decode_r2_frame(a2)
        self.state["timestamp"] = round(time.time(), 3)
        self.state["mode"] = "SIMULATED"
        self.state["raw_pkt1_hex"] = a0.hex()
        self.state["raw_pkt2_hex"] = a2.hex()
        return dict(self.state)

    def dispatch_command(self, action: str, value: Optional[int] = None) -> Dict[str, Any]:
        """
        Queues a control packet for the Bullet (when allow_write is True).
        Also updates simulated state in mock mode so commands can be tested on x64.
        """
        if not self.allow_write:
            raise PermissionError(
                "Write mode is disabled. Start BulletSpy with BULLETSPY_ALLOW_WRITE=1 or --allow-write."
            )

        action = action.strip().lower()
        queued_packets: List[str] = []

        def _encode(opcode: List[int]) -> bytes:
            return prepare_r2_command(opcode) if self.protocol == 2 else bytes(opcode)

        if action == "prs":
            pkt = _encode(CMD_PRS)
            self._cmd_queue.append(pkt)
            queued_packets.append(pkt.hex())
            if self.mock_mode:
                seq = [0x00, 0x02, 0x04, 0x06, 0x08]
                cur = self.state.get("cycle_id", 0x06)
                nxt = seq[(seq.index(cur) + 1) % len(seq)] if cur in seq else 0x06
                self.state["cycle_id"] = nxt
                self.state["cycle"] = STATE_MAP.get(nxt, "ROASTING")

        elif action in ("heater_up", "heater_down"):
            opcode = CMD_HEATER_INCR if action == "heater_up" else CMD_HEATER_DECR
            pkt = _encode(opcode)
            self._cmd_queue.append(pkt)
            queued_packets.append(pkt.hex())
            if self.mock_mode:
                max_p = 14 if self.protocol == 2 else 9
                delta = 1 if action == "heater_up" else -1
                self.state["heater_power"] = max(0, min(max_p, int(self.state["heater_power"]) + delta))

        elif action == "heater" and value is not None:
            max_p = 14 if self.protocol == 2 else 9
            target = max(0, min(max_p, int(value)))
            cur = int(self.state.get("heater_power", 0))
            opcode = CMD_HEATER_INCR if target > cur else CMD_HEATER_DECR
            for _ in range(abs(target - cur)):
                pkt = _encode(opcode)
                self._cmd_queue.append(pkt)
                queued_packets.append(pkt.hex())
            self.state["heater_power"] = target

        elif action in ("fan_up", "fan_down"):
            opcode = CMD_FAN_INCR if action == "fan_up" else CMD_FAN_DECR
            pkt = _encode(opcode)
            self._cmd_queue.append(pkt)
            queued_packets.append(pkt.hex())
            if self.mock_mode:
                delta = 1 if action == "fan_up" else -1
                self.state["fan_speed"] = max(1, min(12, int(self.state["fan_speed"]) + delta))

        elif action == "fan" and value is not None:
            target = max(1, min(12, int(value)))
            cur = int(self.state.get("fan_speed", 1))
            opcode = CMD_FAN_INCR if target > cur else CMD_FAN_DECR
            for _ in range(abs(target - cur)):
                pkt = _encode(opcode)
                self._cmd_queue.append(pkt)
                queued_packets.append(pkt.hex())
            self.state["fan_speed"] = target

        elif action == "drum" and value is not None:
            target = max(1, min(9, int(value)))
            if self.protocol == 2:
                cur = int(self.state.get("drum_speed", 1))
                opcode = CMD_DRUM_INCR if target > cur else CMD_DRUM_DECR
                for _ in range(abs(target - cur)):
                    pkt = _encode(opcode)
                    self._cmd_queue.append(pkt)
                    queued_packets.append(pkt.hex())
            else:
                pkt = bytes([0x32, 0x01, target, 0x00])
                self._cmd_queue.append(pkt)
                queued_packets.append(pkt.hex())
            self.state["drum_speed"] = target

        elif action == "preheat" and value is not None:
            temp = max(20, min(350, int(value)))
            cmd = [0x35, 0x00, (temp >> 8) & 0xFF, temp & 0xFF]
            pkt = _encode(cmd)
            self._cmd_queue.append(pkt)
            queued_packets.append(pkt.hex())

        else:
            raise ValueError(f"Unsupported command action '{action}'")

        return {
            "ok": True,
            "action": action,
            "value": value,
            "queued_packets_hex": queued_packets,
        }


driver = BulletDriver(allow_write=ALLOW_WRITE)
clients: Set[WebSocket] = set()
latest_telemetry: Dict[str, Any] = {}
_poller_task: Optional[asyncio.Task[None]] = None


async def telemetry_loop() -> None:
    global latest_telemetry
    await asyncio.to_thread(driver.connect)
    while True:
        latest_telemetry = await asyncio.to_thread(driver.poll)
        if clients:
            payload = json.dumps(latest_telemetry)
            dead_clients: Set[WebSocket] = set()
            for ws in list(clients):
                try:
                    await ws.send_text(payload)
                except Exception:
                    dead_clients.add(ws)
            clients.difference_update(dead_clients)
        await asyncio.sleep(POLL_INTERVAL)


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global _poller_task, latest_telemetry
    latest_telemetry = driver.poll()
    _poller_task = asyncio.create_task(telemetry_loop())
    try:
        yield
    finally:
        if _poller_task is not None:
            _poller_task.cancel()
            try:
                await _poller_task
            except asyncio.CancelledError:
                pass
        driver.close()


app = FastAPI(title="BulletSpy Telemetry Server", version="0.1.0", lifespan=lifespan)


class CommandRequest(BaseModel):
    action: str
    value: Optional[int] = None


@app.get("/api/state")
async def get_state() -> Dict[str, Any]:
    if not latest_telemetry:
        return driver.poll()
    return latest_telemetry


@app.post("/api/command")
async def post_command(req: CommandRequest) -> Dict[str, Any]:
    try:
        return driver.dispatch_command(req.action, req.value)
    except PermissionError as exc:
        raise HTTPException(status_code=403, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    await websocket.accept()
    clients.add(websocket)
    if latest_telemetry:
        try:
            await websocket.send_text(json.dumps(latest_telemetry))
        except Exception:
            pass
    try:
        while True:
            raw_msg = await websocket.receive_text()
            if driver.allow_write:
                try:
                    msg = json.loads(raw_msg)
                    if isinstance(msg, dict) and "action" in msg:
                        driver.dispatch_command(str(msg["action"]), msg.get("value"))
                except Exception as exc:
                    log.warning("Ignored invalid WS command: %s", exc)
    except WebSocketDisconnect:
        clients.discard(websocket)


DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>BulletSpy // R2 Live Telemetry</title>
<style>
  :root {
    --bg: #0f1115;
    --card: #181b22;
    --border: #262936;
    --text: #e5e7eb;
    --muted: #9ca3af;
    --accent: #f59e0b;
    --ibts: #ef4444;
    --bt: #3b82f6;
    --ok: #10b981;
    --purple: #a855f7;
  }
  * { box-sizing: border-box; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'JetBrains Mono', ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    margin: 0;
    padding: 24px;
  }
  .header {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: center;
    gap: 12px;
    border-bottom: 1px solid #2d3139;
    padding-bottom: 16px;
    margin-bottom: 16px;
  }
  .header-right {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 10px;
  }
  .badge {
    padding: 6px 14px;
    border-radius: 6px;
    font-weight: 700;
    background: #262936;
    color: var(--accent);
    font-size: 1.05rem;
    border: 1px solid #374151;
  }
  .clock-badge {
    padding: 6px 14px;
    border-radius: 6px;
    font-weight: 700;
    background: #14171f;
    color: var(--text);
    font-size: 1.05rem;
    border: 1px solid var(--border);
  }
  .alert-banner {
    display: none;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 14px 20px;
    border-radius: 10px;
    margin-bottom: 16px;
    font-weight: 700;
    font-size: 1.05rem;
    border: 2px solid var(--accent);
    background: rgba(245, 158, 11, 0.14);
    color: #fef3c7;
  }
  .alert-banner.active { display: flex; }
  .alert-banner.critical {
    border-color: var(--ibts);
    background: rgba(239, 68, 68, 0.22);
    color: #fee2e2;
    animation: pulse-border 1s infinite alternate;
  }
  .alert-banner.ready {
    border-color: var(--ok);
    background: rgba(16, 185, 129, 0.18);
    color: #d1fae5;
  }
  @keyframes pulse-border {
    from { box-shadow: 0 0 0 0 rgba(239, 68, 68, 0.45); }
    to { box-shadow: 0 0 18px 4px rgba(239, 68, 68, 0.75); }
  }
  .stage-bar-card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 16px 20px;
    margin-bottom: 16px;
  }
  .stage-top {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: baseline;
    gap: 12px;
    margin-bottom: 12px;
  }
  .stage-title {
    font-size: 1.5rem;
    font-weight: 800;
    letter-spacing: 0.03em;
  }
  .stage-desc {
    color: var(--muted);
    font-size: 0.9rem;
  }
  .stage-segments {
    display: grid;
    grid-template-columns: repeat(7, 1fr);
    gap: 6px;
  }
  .stage-seg {
    background: #101218;
    border: 1px solid #252836;
    border-radius: 6px;
    padding: 7px 6px;
    text-align: center;
    font-size: 0.74rem;
    color: var(--muted);
    transition: all 0.2s ease;
  }
  .stage-seg.active {
    color: #fff;
    font-weight: 700;
    border-color: var(--accent);
    background: #232736;
    box-shadow: 0 0 10px rgba(245, 158, 11, 0.25);
  }
  .stage-seg small {
    display: block;
    font-size: 0.66rem;
    opacity: 0.75;
    margin-top: 2px;
  }
  .target-bar {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 14px 20px;
    margin-bottom: 16px;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  .target-inputs {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 10px;
  }
  input[type="number"], select {
    background: #0f1115;
    color: var(--text);
    border: 1px solid #3b4054;
    border-radius: 6px;
    padding: 6px 10px;
    font-family: inherit;
    font-size: 0.9rem;
  }
  input[type="number"] { width: 88px; font-weight: 700; color: var(--accent); }
  .preset-pill {
    font-size: 0.78rem;
    padding: 5px 10px;
    border-radius: 999px;
    background: #1e2230;
    border: 1px solid #32374a;
  }
  .preset-pill.active {
    border-color: var(--accent);
    color: var(--accent);
    background: rgba(245, 158, 11, 0.12);
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
    gap: 16px;
  }
  .card {
    background: var(--card);
    padding: 18px 20px;
    border-radius: 10px;
    border: 1px solid var(--border);
  }
  .label {
    color: var(--muted);
    font-size: 0.8rem;
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
  .val {
    font-size: 2.35rem;
    font-weight: 700;
    margin-top: 8px;
    line-height: 1.1;
  }
  .sub {
    font-size: 0.9rem;
    color: var(--muted);
    margin-top: 6px;
  }
  .bt { color: var(--bt); }
  .ibts { color: var(--ibts); }
  .chart-card {
    margin-top: 16px;
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 16px 20px;
  }
  .chart-header {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: center;
    gap: 8px;
    margin-bottom: 10px;
  }
  .legend {
    display: flex;
    flex-wrap: wrap;
    gap: 16px;
    font-size: 0.8rem;
    color: var(--muted);
  }
  .legend span::before { content: "● "; }
  .leg-ibts::before { color: var(--ibts); }
  .leg-bt::before { color: var(--bt); }
  .leg-ror::before { color: var(--accent); }
  .leg-target::before { color: var(--ok); content: "― "; }
  canvas {
    width: 100%;
    height: 180px;
    display: block;
    background: #0b0d11;
    border-radius: 6px;
    border: 1px solid #1f2330;
  }
  .controls {
    margin-top: 16px;
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 14px 20px;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  .monster-grid {
    margin-top: 16px;
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
    gap: 16px;
  }
  .monster-card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 16px 20px;
  }
  .monster-header {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: center;
    gap: 8px;
    margin-bottom: 10px;
  }
  .ai-pill {
    font-size: 0.72rem;
    font-weight: 700;
    padding: 3px 9px;
    border-radius: 999px;
    background: #232736;
    color: var(--purple);
    border: 1px solid rgba(168, 85, 247, 0.4);
  }
  .monster-status-box {
    background: #10131a;
    border-left: 4px solid var(--ok);
    border-radius: 6px;
    padding: 12px 14px;
    margin-top: 8px;
  }
  .monster-headline {
    font-weight: 700;
    font-size: 1rem;
    margin-bottom: 4px;
  }
  .monster-suggestion {
    font-size: 0.86rem;
    color: var(--muted);
    line-height: 1.4;
  }
  .log-list {
    margin-top: 10px;
    max-height: 135px;
    overflow-y: auto;
    font-size: 0.8rem;
    background: #0b0d11;
    border: 1px solid #1f2330;
    border-radius: 6px;
    padding: 8px 12px;
  }
  .log-item {
    padding: 4px 0;
    border-bottom: 1px solid #181b24;
    display: flex;
    justify-content: space-between;
    gap: 8px;
  }
  .log-item:last-child { border-bottom: none; }
  input[type="text"] {
    background: #0f1115;
    color: var(--text);
    border: 1px solid #3b4054;
    border-radius: 6px;
    padding: 6px 10px;
    font-family: inherit;
    font-size: 0.85rem;
    flex: 1;
    min-width: 160px;
  }
  .btn-group {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
  }
  button {
    background: #262936;
    color: var(--text);
    border: 1px solid #3b4054;
    border-radius: 6px;
    padding: 6px 12px;
    font-family: inherit;
    font-size: 0.85rem;
    cursor: pointer;
  }
  button:hover:not(:disabled) {
    border-color: var(--accent);
    color: var(--accent);
  }
  button:disabled {
    opacity: 0.45;
    cursor: not-allowed;
  }
  pre {
    background: #090a0d;
    padding: 14px;
    border-radius: 8px;
    border: 1px solid var(--border);
    overflow-x: auto;
    font-size: 0.78rem;
    line-height: 1.45;
    color: #9ca3af;
    margin-top: 8px;
  }
</style>
</head>
<body>
  <div class="header">
    <div>
      <h1 style="margin:0; font-size:1.45rem;">BULLETSPY // R2 TELEMETRY</h1>
      <small id="conn-status" style="color:var(--ok);">Connecting WebSocket...</small>
    </div>
    <div class="header-right">
      <button id="audio-toggle" title="Toggle Web Audio synthesizer chimes">🔊 Chimes: ON</button>
      <button id="test-preheat-btn" title="Preview Preheat Complete chime">🔔 Test Preheat</button>
      <button id="test-drop-btn" title="Preview Target Reached Drop alarm">🚨 Test Drop</button>
      <div id="roast-clock" class="clock-badge">00:00</div>
      <div id="cycle-badge" class="badge">WAITING</div>
    </div>
  </div>

  <!-- Dynamic Visual & Audio Event Banner -->
  <div id="alert-banner" class="alert-banner">
    <div id="alert-text">EVENT ALERT</div>
    <button id="ack-alert-btn">Acknowledge / Silence</button>
  </div>

  <!-- Bean Roast Level & Chemical Phase Bar -->
  <div class="stage-bar-card">
    <div class="stage-top">
      <div>
        <div class="label">Bean Roast Level // Chemical Stage</div>
        <div id="roast-stage-label" class="stage-title" style="color:var(--accent);">WAITING FOR TELEMETRY</div>
      </div>
      <div id="roast-stage-desc" class="stage-desc">--</div>
    </div>
    <div class="stage-segments">
      <div class="stage-seg" data-zone="DRYING">Drying<small>&lt;150°C</small></div>
      <div class="stage-seg" data-zone="MAILLARD">Maillard<small>150–175°C</small></div>
      <div class="stage-seg" data-zone="PRE-1ST CRACK">Pre-FC<small>175–196°C</small></div>
      <div class="stage-seg" data-zone="FIRST CRACK">1st Crack<small>196–205°C</small></div>
      <div class="stage-seg" data-zone="CITY">City<small>205–214°C</small></div>
      <div class="stage-seg" data-zone="FULL CITY">Full City<small>214–220°C</small></div>
      <div class="stage-seg" data-zone="VIENNA">Vienna / Dark<small>220°C+</small></div>
    </div>
  </div>

  <!-- Target Bean Temperature & Preset Selector -->
  <div class="target-bar">
    <div class="target-inputs">
      <span class="label" style="margin-right:4px;">DROP TARGET:</span>
      <input type="number" id="target-temp-input" value="215" min="150" max="245" step="0.5">
      <span style="font-size:0.9rem; color:var(--muted);">°C on</span>
      <select id="target-sensor-select">
        <option value="bean_temp_c">Bean Probe (BT)</option>
        <option value="drum_temp_ibts_c">Drum IBTS (DT)</option>
      </select>
      <span id="target-delta" class="sub" style="margin:0 0 0 8px; font-weight:700;">Δ --.- °C to drop</span>
    </div>
    <div class="btn-group">
      <button class="preset-pill" data-temp="200">Light / FC (200°C)</button>
      <button class="preset-pill" data-temp="209">City (209°C)</button>
      <button class="preset-pill active" data-temp="215">Full City (215°C)</button>
      <button class="preset-pill" data-temp="221">Full City+ (221°C)</button>
      <button class="preset-pill" data-temp="226">Vienna (226°C)</button>
    </div>
  </div>

  <div class="grid">
    <div class="card">
      <div class="label">Drum Temp (IBTS)</div>
      <div id="dt" class="val ibts">--.- °C</div>
      <div id="dt-ror" class="sub">RoR: --.- °C/m</div>
    </div>
    <div class="card">
      <div class="label">Bean Temp (Probe)</div>
      <div id="bt" class="val bt">--.- °C</div>
      <div id="bt-ror" class="sub">RoR: --.- °C/m</div>
    </div>
    <div class="card">
      <div class="label">Induction Power</div>
      <div id="power" class="val">P-</div>
      <div id="volts" class="sub">Line: ---.- V | --- W</div>
    </div>
    <div class="card">
      <div class="label">Exhaust Fan / Drum</div>
      <div id="fan" class="val">F- / D-</div>
      <div id="rpm" class="sub">Blower: ---- RPM | Drum: -- RPM</div>
    </div>
    <div class="card">
      <div class="label">Exhaust / Inlet Air</div>
      <div id="exhaust" class="val">--.- °C</div>
      <div id="inlet" class="sub">Inlet: --.- °C | ΔP: --.- Pa</div>
    </div>
    <div class="card">
      <div class="label">Roast Energy / Status</div>
      <div id="energy" class="val">--.- Wh</div>
      <div id="mode" class="sub">Source: --</div>
    </div>
  </div>

  <div class="chart-card">
    <div class="chart-header">
      <span class="label">LIVE TELEMETRY TRACE (ROLLING WINDOW)</span>
      <div class="legend">
        <span class="leg-ibts">IBTS (°C)</span>
        <span class="leg-bt">Bean Probe (°C)</span>
        <span class="leg-ror">IBTS RoR (°C/m)</span>
        <span class="leg-target">Target Drop Temp</span>
      </div>
    </div>
    <canvas id="trace-canvas" width="900" height="180"></canvas>
  </div>

  <div class="controls">
    <div>
      <span class="label">COMMAND DISPATCH HOOKS</span>
      <div id="write-status" class="sub">Read-only mode active (safe polling only)</div>
    </div>
    <div class="btn-group">
      <button class="cmd-btn" data-action="prs" disabled>PRS (Advance Cycle)</button>
      <button class="cmd-btn" data-action="heater_down" disabled>Power -</button>
      <button class="cmd-btn" data-action="heater_up" disabled>Power +</button>
      <button class="cmd-btn" data-action="fan_down" disabled>Fan -</button>
      <button class="cmd-btn" data-action="fan_up" disabled>Fan +</button>
    </div>
  </div>

  <!-- On-Device AI Roast Monster & Hands-Free Voice / Milestone Logger -->
  <div class="monster-grid">
    <div class="monster-card">
      <div class="monster-header">
        <div>
          <span class="label">ROAST MONSTER // CURVE ANOMALY RADAR</span>
          <span id="ai-engine-pill" class="ai-pill">Physics Engine (Fallback)</span>
        </div>
        <div class="btn-group">
          <button id="init-ai-btn" title="Enable Chrome Built-in Gemini Nano (LanguageModel API)">✨ Enable Gemini Nano</button>
          <button id="run-monster-btn" title="Run immediate curve evaluation">Analyze Now</button>
        </div>
      </div>
      <div id="monster-box" class="monster-status-box">
        <div id="monster-headline" class="monster-headline">[OPTIMAL] Waiting for 10s telemetry window...</div>
        <div id="monster-suggestion" class="monster-suggestion">
          Monitors 30s RoR slope for RoR crashes, post-crack flicks, and pre-FC stalls every 10s.
        </div>
      </div>
    </div>

    <div class="monster-card">
      <div class="monster-header">
        <span class="label">HANDS-FREE VOICE LOG &amp; BATCH SUMMARY</span>
        <div class="btn-group">
          <button id="voice-log-btn" title="Speak a roast note via Web Speech API">🎤 Voice Note</button>
          <button id="speak-temp-btn" title="Read current Bean Temp &amp; RoR aloud">🗣️ Callout Temp</button>
          <button id="export-md-btn" title="Copy Markdown Roast Summary to clipboard">📋 Copy Batch MD</button>
        </div>
      </div>
      <div style="display:flex; gap:8px; margin-top:6px;">
        <input type="text" id="manual-note-input" placeholder="Log sensory note (e.g. 'Yellowing, sweet hay', '1st crack rolling')...">
        <button id="add-note-btn">Log</button>
      </div>
      <div id="roast-log-list" class="log-list">
        <div class="sub" style="margin:0;">No milestones logged yet — click 🎤 Voice Note or type above.</div>
      </div>
    </div>
  </div>

  <h3 style="margin-top:24px; margin-bottom:4px; color:var(--muted); font-size:0.85rem;">
    RAW USB PAYLOAD INSPECTOR (64B FRAMES)
  </h3>
  <pre id="raw-hex">Waiting for frames...</pre>

<script>
  const history = [];
  const MAX_POINTS = 240; // 60s at 4Hz
  const canvas = document.getElementById('trace-canvas');
  const ctx = canvas.getContext('2d');

  // --- AUDIO SYNTHESIZER (Web Audio API - zero external dependencies) ---
  let audioCtx = null;
  let audioEnabled = true;
  let alarmInterval = null;

  function ensureAudio() {
    if (!audioCtx) {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      if (AudioContextClass) audioCtx = new AudioContextClass();
    }
    if (audioCtx && audioCtx.state === 'suspended') {
      audioCtx.resume();
    }
  }

  function playTone(freq, startOffset, duration, type = 'sine', peakGain = 0.18) {
    if (!audioEnabled) return;
    ensureAudio();
    if (!audioCtx) return;

    const now = audioCtx.currentTime + startOffset;
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = type;
    osc.frequency.setValueAtTime(freq, now);

    gain.gain.setValueAtTime(0.0001, now);
    gain.gain.exponentialRampToValueAtTime(peakGain, now + 0.02);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + duration);

    osc.connect(gain);
    gain.connect(audioCtx.destination);
    osc.start(now);
    osc.stop(now + duration + 0.02);
  }

  function playChime(kind) {
    if (!audioEnabled) return;
    if (kind === 'preheat') {
      // Ascending C5 -> E5 -> G5 -> C6 triad chime for Preheat Complete / Charge Ready
      playTone(523.25, 0.00, 0.22, 'triangle', 0.20);
      playTone(659.25, 0.14, 0.22, 'triangle', 0.20);
      playTone(783.99, 0.28, 0.25, 'triangle', 0.20);
      playTone(1046.50, 0.44, 0.55, 'sine', 0.24);
    } else if (kind === 'milestone') {
      // Crisp 2-note chime for Roast Stage transitions (e.g. First Crack)
      playTone(587.33, 0.00, 0.18, 'sine', 0.15);
      playTone(880.00, 0.14, 0.35, 'sine', 0.18);
    } else if (kind === 'approaching') {
      // Gentle double-pip heads up (within 2.5C of target)
      playTone(740.00, 0.00, 0.14, 'sine', 0.14);
      playTone(740.00, 0.18, 0.18, 'sine', 0.14);
    } else if (kind === 'target') {
      // Urgent harmonic bell chime for Target Bean Temp Reached (DROP NOW!)
      playTone(783.99, 0.00, 0.22, 'triangle', 0.25);
      playTone(1174.66, 0.00, 0.22, 'sine', 0.18);
      playTone(1046.50, 0.22, 0.22, 'triangle', 0.25);
      playTone(1567.98, 0.22, 0.45, 'sine', 0.22);
      playTone(1046.50, 0.50, 0.55, 'triangle', 0.25);
    }
  }

  // --- ALERT BANNER & STATE TRACKING ---
  const bannerEl = document.getElementById('alert-banner');
  const bannerTextEl = document.getElementById('alert-text');
  let lastCycle = null;
  let lastStageShort = null;
  let approachingFired = false;
  let targetReachedFired = false;
  let targetAcknowledged = false;

  function showBanner(msg, styleClass = '', repeatTargetAlarm = false) {
    bannerEl.className = `alert-banner active ${styleClass}`;
    bannerTextEl.textContent = msg;
    if (alarmInterval) {
      clearInterval(alarmInterval);
      alarmInterval = null;
    }
    if (repeatTargetAlarm) {
      alarmInterval = setInterval(() => {
        if (!targetAcknowledged && audioEnabled) playChime('target');
      }, 3200);
    }
  }

  function clearBanner() {
    bannerEl.className = 'alert-banner';
    targetAcknowledged = true;
    if (alarmInterval) {
      clearInterval(alarmInterval);
      alarmInterval = null;
    }
  }

  document.getElementById('ack-alert-btn').addEventListener('click', clearBanner);

  // Audio controls in header
  const audioToggleBtn = document.getElementById('audio-toggle');
  audioToggleBtn.addEventListener('click', () => {
    audioEnabled = !audioEnabled;
    if (audioEnabled) ensureAudio();
    audioToggleBtn.textContent = audioEnabled ? '🔊 Chimes: ON' : '🔇 Chimes: MUTED';
    audioToggleBtn.style.color = audioEnabled ? 'var(--text)' : 'var(--muted)';
  });

  document.getElementById('test-preheat-btn').addEventListener('click', () => {
    audioEnabled = true;
    audioToggleBtn.textContent = '🔊 Chimes: ON';
    playChime('preheat');
    showBanner('🔔 PREHEAT COMPLETE — DRUM AT CHARGE TEMPERATURE (READY TO DROP GREEN BEANS)', 'ready', false);
  });

  document.getElementById('test-drop-btn').addEventListener('click', () => {
    audioEnabled = true;
    audioToggleBtn.textContent = '🔊 Chimes: ON';
    targetAcknowledged = false;
    playChime('target');
    const tgt = parseFloat(document.getElementById('target-temp-input').value) || 215;
    showBanner(`🚨 TARGET TEMPERATURE REACHED (${tgt.toFixed(1)} °C) — ROAST DONE, DROP BEANS NOW!`, 'critical', true);
  });

  // Target temperature & presets
  const targetInput = document.getElementById('target-temp-input');
  const sensorSelect = document.getElementById('target-sensor-select');
  const savedTarget = localStorage.getItem('bulletspy_target_c');
  if (savedTarget) targetInput.value = savedTarget;

  function resetTargetTriggers() {
    approachingFired = false;
    targetReachedFired = false;
    targetAcknowledged = false;
  }

  targetInput.addEventListener('change', () => {
    localStorage.setItem('bulletspy_target_c', targetInput.value);
    resetTargetTriggers();
    document.querySelectorAll('.preset-pill').forEach(p => {
      p.classList.toggle('active', p.dataset.temp === targetInput.value);
    });
  });

  sensorSelect.addEventListener('change', resetTargetTriggers);

  document.querySelectorAll('.preset-pill').forEach(pill => {
    pill.addEventListener('click', () => {
      targetInput.value = pill.dataset.temp;
      localStorage.setItem('bulletspy_target_c', targetInput.value);
      resetTargetTriggers();
      document.querySelectorAll('.preset-pill').forEach(p => p.classList.remove('active'));
      pill.classList.add('active');
    });
  });

  function drawChart() {
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    // Subtle horizontal grid lines
    ctx.strokeStyle = '#1b1f2a';
    ctx.lineWidth = 1;
    for (let i = 1; i < 4; i++) {
      const y = (h / 4) * i;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }

    // Draw Target Temperature dashed reference line
    const targetTemp = parseFloat(targetInput.value) || 215;
    const minVal = 50, maxVal = 260;
    const targetNorm = (targetTemp - minVal) / (maxVal - minVal);
    const targetY = h - Math.max(0, Math.min(1, targetNorm)) * (h - 16) - 8;
    ctx.save();
    ctx.strokeStyle = '#10b981';
    ctx.lineWidth = 1.5;
    ctx.setLineDash([6, 4]);
    ctx.beginPath();
    ctx.moveTo(0, targetY);
    ctx.lineTo(w, targetY);
    ctx.stroke();
    ctx.restore();

    if (history.length < 2) return;

    function plotLine(getter, lo, hi, color) {
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      history.forEach((pt, idx) => {
        const x = (idx / (MAX_POINTS - 1)) * w;
        const norm = (getter(pt) - lo) / (hi - lo || 1);
        const y = h - Math.max(0, Math.min(1, norm)) * (h - 16) - 8;
        if (idx === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();
    }

    plotLine(p => p.drum_temp_ibts_c, minVal, maxVal, '#ef4444');
    plotLine(p => p.bean_temp_c, minVal, maxVal, '#3b82f6');
    plotLine(p => p.drum_ror_c_min, -5, 30, '#f59e0b');
  }

  async function sendCmd(action) {
    try {
      await fetch('/api/command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action })
      });
    } catch (e) {
      console.error('Command error:', e);
    }
  }

  document.querySelectorAll('.cmd-btn').forEach(btn => {
    btn.addEventListener('click', () => sendCmd(btn.dataset.action));
  });

  function evaluateAlerts(d) {
    // 1. Preheat Complete detection (transition into CHARGE / 0x04)
    if (lastCycle !== null && lastCycle !== 'CHARGE' && d.cycle === 'CHARGE') {
      playChime('preheat');
      showBanner('🔔 PREHEAT COMPLETE — READY TO CHARGE GREEN BEANS!', 'ready', false);
    }
    // Auto-clear drop alarm if roaster enters COOLING/COOLDOWN
    if (lastCycle !== d.cycle && (d.cycle === 'COOLING' || d.cycle === 'COOLDOWN')) {
      clearBanner();
    }
    lastCycle = d.cycle;

    // 2. Roast Stage Milestone detection (e.g. entering First Crack)
    if (d.cycle === 'ROASTING' && lastStageShort && lastStageShort !== d.roast_stage_short) {
      if (d.roast_stage_short === 'FIRST CRACK') {
        playChime('milestone');
        if (!targetReachedFired) {
          showBanner('🔥 FIRST CRACK REACHED — WATCH DEVELOPMENT TIME & RoR', '', false);
        }
      } else if (d.roast_stage_short === 'MAILLARD') {
        playChime('milestone');
      }
    }
    lastStageShort = d.roast_stage_short;

    // 3. Target Bean Temperature evaluation
    const targetTemp = parseFloat(targetInput.value) || 215;
    const sensorKey = sensorSelect.value;
    const currentTemp = Number(d[sensorKey] || 0);
    const delta = targetTemp - currentTemp;
    const deltaEl = document.getElementById('target-delta');

    if (delta <= 0) {
      deltaEl.textContent = `🎯 TARGET REACHED (+${Math.abs(delta).toFixed(1)} °C)`;
      deltaEl.style.color = '#ef4444';
      if (d.cycle === 'ROASTING' && !targetReachedFired) {
        targetReachedFired = true;
        targetAcknowledged = false;
        playChime('target');
        showBanner(
          `🚨 TARGET TEMPERATURE REACHED (${currentTemp.toFixed(1)} °C >= ${targetTemp.toFixed(1)} °C) — ROAST DONE, DROP BEANS!`,
          'critical',
          true
        );
      }
    } else {
      deltaEl.textContent = `Δ ${delta.toFixed(1)} °C to drop`;
      deltaEl.style.color = delta <= 2.5 ? '#f59e0b' : '#9ca3af';
      if (d.cycle === 'ROASTING' && delta <= 2.5 && !approachingFired) {
        approachingFired = true;
        playChime('approaching');
        showBanner(`⚡ APPROACHING TARGET — ${delta.toFixed(1)} °C TO DROP (${targetTemp.toFixed(1)} °C)`, '', false);
      }
      // Reset if temperature drops well below target (e.g. new roast)
      if (delta > 6.0) {
        approachingFired = false;
        targetReachedFired = false;
      }
    }
  }

  // --- CHROME BUILT-IN AI (GEMINI NANO) ROAST MONSTER + HEURISTIC FALLBACK ---
  let baseAiSession = null;
  let lastRorSampleTime = 0;
  let lastMonsterEvalTime = 0;
  const rorHistory = []; // Sampled every 5s (keeps last 12 samples = 60s)
  const roastMilestones = [];

  const roastMonsterSchema = {
    type: 'object',
    properties: {
      status: { type: 'string', enum: ['OPTIMAL', 'WATCH', 'WARNING'] },
      headline: { type: 'string' },
      suggestion: { type: 'string' }
    },
    required: ['status', 'headline', 'suggestion'],
    additionalProperties: false
  };

  function renderMonsterAdvice(advice, sourceLabel) {
    const box = document.getElementById('monster-box');
    const headEl = document.getElementById('monster-headline');
    const sugEl = document.getElementById('monster-suggestion');
    const pillEl = document.getElementById('ai-engine-pill');

    const colorMap = { OPTIMAL: '#10b981', WATCH: '#f59e0b', WARNING: '#ef4444' };
    box.style.borderLeftColor = colorMap[advice.status] || '#10b981';
    // Use textContent strictly per Chrome LanguageModel security best practices
    headEl.textContent = `[${advice.status}] ${advice.headline}`;
    headEl.style.color = colorMap[advice.status] || '#e5e7eb';
    sugEl.textContent = advice.suggestion;
    if (sourceLabel) pillEl.textContent = sourceLabel;
  }

  async function initChromeAI() {
    const pillEl = document.getElementById('ai-engine-pill');
    const btn = document.getElementById('init-ai-btn');
    if (!('LanguageModel' in self)) {
      pillEl.textContent = 'Physics Engine (Nano Unavailable)';
      renderMonsterAdvice({
        status: 'WATCH',
        headline: 'Chrome Prompt API (LanguageModel) not detected in this browser',
        suggestion: 'Running deterministic Roast Monster Physics Engine. To enable Gemini Nano, open in Chrome 138+ on localhost/HTTPS with #prompt-api-for-gemini-nano enabled.'
      });
      return false;
    }
    try {
      const availability = await LanguageModel.availability();
      if (availability === 'unavailable') {
        pillEl.textContent = 'Physics Engine (Nano Unavailable)';
        return false;
      }
      btn.disabled = true;
      btn.textContent = 'Loading Nano...';
      baseAiSession = await LanguageModel.create({
        initialPrompts: [{
          role: 'system',
          content:
            'You are Roast Monster, an expert specialty coffee roasting advisor monitoring an Aillio Bullet R2. ' +
            'Analyze the telemetry snapshot. Look for RoR crashes (rapid drop in Rate of Rise), ' +
            'RoR flicks (sudden late spike), stalling heat before First Crack (~196C-202C), or excessive drum heat. ' +
            'Be terse, practical, and technical.'
        }],
        monitor(m) {
          m.addEventListener('downloadprogress', (e) => {
            const pct = Math.round((e.loaded || 0) * 100);
            pillEl.textContent = `Downloading Nano (${pct}%)...`;
          });
        }
      });
      pillEl.textContent = 'Gemini Nano (On-Device)';
      pillEl.style.color = '#10b981';
      btn.textContent = '✨ Nano Active';
      if (history.length > 0) evaluateRoastState(history[history.length - 1], true);
      return true;
    } catch (err) {
      console.warn('Gemini Nano init fallback:', err);
      pillEl.textContent = 'Physics Engine (Fallback)';
      btn.disabled = false;
      btn.textContent = '✨ Enable Gemini Nano';
      return false;
    }
  }

  function evaluateHeuristicMonster(d) {
    const trend = rorHistory.slice(-6);
    const rorDelta = trend.length >= 2 ? (trend[trend.length - 1] - trend[0]) : 0;

    if (d.cycle !== 'ROASTING') {
      return {
        status: 'OPTIMAL',
        headline: `Roaster in ${d.cycle} state (${d.roast_stage_short})`,
        suggestion: d.cycle === 'CHARGE'
          ? 'Drum thermal soak ready. Charge green beans and verify initial P/F settings.'
          : 'Monitoring telemetry stream.'
      };
    }
    if (d.drum_temp_ibts_c > 248) {
      return {
        status: 'WARNING',
        headline: `High IBTS surface temp (${d.drum_temp_ibts_c.toFixed(1)} °C)`,
        suggestion: 'Reduce induction power (P) or step up exhaust fan (F) to prevent tipping/scorching.'
      };
    }
    if (d.bean_temp_c >= 192 && rorDelta < -3.5) {
      return {
        status: 'WARNING',
        headline: `RoR Crash Detected (${rorDelta.toFixed(1)} °C/m over 30s)`,
        suggestion: 'Exothermic steam venting is collapsing RoR. Avoid dropping power too aggressively or increase airflow slightly.'
      };
    }
    if (d.bean_temp_c >= 198 && rorDelta > 2.2) {
      return {
        status: 'WATCH',
        headline: `Late RoR Flick (+${rorDelta.toFixed(1)} °C/m upswing)`,
        suggestion: 'RoR is accelerating in development. Step fan up +1 or trim heater power -1 to keep a steadily declining RoR.'
      };
    }
    if (d.bean_temp_c < 194 && d.bean_ror_c_min < 4.0) {
      return {
        status: 'WATCH',
        headline: `Low Pre-Crack Momentum (RoR ${d.bean_ror_c_min.toFixed(1)} °C/m)`,
        suggestion: 'Risk of stalling before First Crack (baked flavors). Consider +1 induction power if IBTS headroom allows.'
      };
    }
    return {
      status: 'OPTIMAL',
      headline: `Curve tracking smoothly in ${d.roast_stage_short} (${d.bean_ror_c_min.toFixed(1)} °C/m)`,
      suggestion: `30s RoR Δ: ${rorDelta >= 0 ? '+' : ''}${rorDelta.toFixed(1)} °C/m | Power P${d.heater_power} · Fan F${d.fan_speed} · Drum D${d.drum_speed}`
    };
  }

  async function evaluateRoastState(latestData, force = false) {
    if (!latestData) return;
    const now = Date.now();
    if (!force && now - lastMonsterEvalTime < 10000) return;
    lastMonsterEvalTime = now;

    if (baseAiSession && latestData.cycle === 'ROASTING') {
      const turnSession = await baseAiSession.clone();
      try {
        const promptText = JSON.stringify({
          roast_clock: latestData.roast_clock,
          stage: latestData.roast_stage_short,
          ibts_drum_c: latestData.drum_temp_ibts_c,
          bean_probe_c: latestData.bean_temp_c,
          bean_ror: latestData.bean_ror_c_min,
          ror_30s_trend: rorHistory.slice(-6),
          power: `P${latestData.heater_power}`,
          fan: `F${latestData.fan_speed}`,
          drum: `D${latestData.drum_speed}`
        });
        const rawResult = await turnSession.prompt(
          `Current Roast Telemetry: ${promptText}`,
          { responseConstraint: roastMonsterSchema }
        );
        const advice = JSON.parse(rawResult);
        renderMonsterAdvice(advice, 'Gemini Nano (On-Device)');
        return;
      } catch (err) {
        console.warn('Gemini Nano prompt error, using physics fallback:', err);
      } finally {
        turnSession.destroy();
      }
    }

    const fallbackAdvice = evaluateHeuristicMonster(latestData);
    renderMonsterAdvice(fallbackAdvice, baseAiSession ? 'Gemini Nano (On-Device)' : 'Physics Engine (Active)');
  }

  document.getElementById('init-ai-btn').addEventListener('click', initChromeAI);
  document.getElementById('run-monster-btn').addEventListener('click', () => {
    if (history.length > 0) evaluateRoastState(history[history.length - 1], true);
  });

  // --- HANDS-FREE VOICE LOGGING & MARKDOWN BATCH EXPORT ---
  function logRoastMilestone(noteText, source = 'Manual') {
    if (!noteText || !noteText.trim()) return;
    const d = history.length > 0 ? history[history.length - 1] : {};
    const entry = {
      clock: d.roast_clock || '00:00',
      bt: d.bean_temp_c !== undefined ? d.bean_temp_c.toFixed(1) : '--',
      ibts: d.drum_temp_ibts_c !== undefined ? d.drum_temp_ibts_c.toFixed(1) : '--',
      ror: d.bean_ror_c_min !== undefined ? d.bean_ror_c_min.toFixed(1) : '--',
      stage: d.roast_stage_short || 'ROAST',
      note: noteText.trim(),
      source
    };
    roastMilestones.push(entry);

    const listEl = document.getElementById('roast-log-list');
    listEl.textContent = '';
    roastMilestones.slice().reverse().forEach(item => {
      const row = document.createElement('div');
      row.className = 'log-item';
      const left = document.createElement('span');
      left.textContent = `[${item.clock}] ${item.note}`;
      const right = document.createElement('span');
      right.style.color = '#9ca3af';
      right.textContent = `BT ${item.bt}°C | IBTS ${item.ibts}°C (${item.stage})`;
      row.appendChild(left);
      row.appendChild(right);
      listEl.appendChild(row);
    });
  }

  document.getElementById('add-note-btn').addEventListener('click', () => {
    const inp = document.getElementById('manual-note-input');
    logRoastMilestone(inp.value, 'Note');
    inp.value = '';
  });

  document.getElementById('manual-note-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') document.getElementById('add-note-btn').click();
  });

  document.getElementById('speak-temp-btn').addEventListener('click', () => {
    if (!('speechSynthesis' in window) || history.length === 0) return;
    const d = history[history.length - 1];
    const utter = new SpeechSynthesisUtterance(
      `${d.roast_stage_short}. Bean temperature ${d.bean_temp_c.toFixed(1)} degrees. Rate of rise ${d.bean_ror_c_min.toFixed(1)}.`
    );
    window.speechSynthesis.speak(utter);
  });

  document.getElementById('voice-log-btn').addEventListener('click', () => {
    const SpeechRec = window.SpeechRecognition || window.webkitSpeechRecognition;
    const btn = document.getElementById('voice-log-btn');
    if (!SpeechRec) {
      logRoastMilestone('Web Speech API not supported in this browser — use text input.', 'System');
      return;
    }
    const rec = new SpeechRec();
    rec.lang = 'en-US';
    rec.interimResults = false;
    rec.maxAlternatives = 1;
    btn.textContent = '🎙️ Listening...';
    rec.onresult = (event) => {
      const transcript = event.results[0][0].transcript;
      logRoastMilestone(transcript, 'Voice');
      if ('speechSynthesis' in window && history.length > 0) {
        const d = history[history.length - 1];
        window.speechSynthesis.speak(
          new SpeechSynthesisUtterance(`Logged ${transcript} at ${d.bean_temp_c.toFixed(1)} degrees`)
        );
      }
    };
    rec.onend = () => { btn.textContent = '🎤 Voice Note'; };
    rec.onerror = () => { btn.textContent = '🎤 Voice Note'; };
    rec.start();
  });

  document.getElementById('export-md-btn').addEventListener('click', async () => {
    const d = history.length > 0 ? history[history.length - 1] : {};
    const targetTemp = parseFloat(targetInput.value) || 215;
    const lines = [
      `# BulletSpy Roast Batch Log`,
      `- **Model:** ${d.model || 'Aillio Bullet R2'}`,
      `- **Final Stage:** ${d.roast_stage || '--'}`,
      `- **Elapsed Clock:** ${d.roast_clock || '00:00'}`,
      `- **Bean Probe / IBTS:** ${d.bean_temp_c ?? '--'} °C / ${d.drum_temp_ibts_c ?? '--'} °C`,
      `- **Target Drop Temp:** ${targetTemp.toFixed(1)} °C`,
      `- **Energy Used:** ${d.energy_used_wh ?? '--'} Wh`,
      ``,
      `## Logged Milestones`,
      ...(roastMilestones.length
        ? roastMilestones.map(m => `- **${m.clock}** — ${m.note} *(BT ${m.bt}°C, IBTS ${m.ibts}°C, RoR ${m.ror}°C/m, ${m.stage})*`)
        : ['- *(No manual/voice milestones logged)*'])
    ];
    const md = lines.join('\\n');
    try {
      await navigator.clipboard.writeText(md);
      const btn = document.getElementById('export-md-btn');
      btn.textContent = '✅ Copied MD!';
      setTimeout(() => { btn.textContent = '📋 Copy Batch MD'; }, 2000);
    } catch (e) {
      console.log(md);
    }
  });

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(`${proto}//${location.host}/ws`);
    const status = document.getElementById('conn-status');

    ws.onopen = () => {
      status.textContent = 'LIVE STREAM CONNECTED (4Hz)';
      status.style.color = '#10b981';
    };
    ws.onclose = () => {
      status.textContent = 'DISCONNECTED - RETRYING...';
      status.style.color = '#ef4444';
      setTimeout(connect, 2000);
    };
    ws.onmessage = (event) => {
      const d = JSON.parse(event.data);
      document.getElementById('cycle-badge').textContent = d.cycle;
      document.getElementById('roast-clock').textContent = d.roast_clock || '00:00';
      document.getElementById('dt').textContent = `${d.drum_temp_ibts_c.toFixed(1)} °C`;
      document.getElementById('dt-ror').textContent = `RoR: ${d.drum_ror_c_min.toFixed(1)} °C/m`;
      document.getElementById('bt').textContent = `${d.bean_temp_c.toFixed(1)} °C`;
      document.getElementById('bt-ror').textContent = `RoR: ${d.bean_ror_c_min.toFixed(1)} °C/m`;
      document.getElementById('power').textContent = `P${d.heater_power}`;
      document.getElementById('volts').textContent =
        `Line: ${d.voltage_v.toFixed(1)} V | ${d.active_power_w.toFixed(0)} W`;
      document.getElementById('fan').textContent = `F${d.fan_speed} / D${d.drum_speed}`;
      document.getElementById('rpm').textContent =
        `Blower: ${d.fan_rpm} RPM | Drum: ${d.drum_rpm || '--'} RPM`;
      document.getElementById('exhaust').textContent = `${d.exhaust_temp_c.toFixed(1)} °C`;
      document.getElementById('inlet').textContent =
        `Inlet: ${(d.inlet_temp_c || 0).toFixed(1)} °C | ΔP: ${(d.diff_pressure_pa || 0).toFixed(1)} Pa`;
      document.getElementById('energy').textContent = `${(d.energy_used_wh || 0).toFixed(1)} Wh`;
      document.getElementById('mode').textContent = `Source: ${d.mode} (${d.model || 'Bullet'})`;

      // Update Bean Roast Stage label & segment bar
      const stageLabelEl = document.getElementById('roast-stage-label');
      const stageDescEl = document.getElementById('roast-stage-desc');
      stageLabelEl.textContent = d.roast_stage || '--';
      stageLabelEl.style.color = d.roast_stage_color || '#f59e0b';
      stageDescEl.textContent = d.roast_stage_desc || '';

      const activeZone = d.roast_stage_short === 'FRENCH' || d.roast_stage_short === 'ITALIAN / DROP!'
        ? 'VIENNA'
        : d.roast_stage_short;
      document.querySelectorAll('.stage-seg').forEach(seg => {
        seg.classList.toggle('active', seg.dataset.zone === activeZone);
      });

      evaluateAlerts(d);

      const writeStatus = document.getElementById('write-status');
      document.querySelectorAll('.cmd-btn').forEach(btn => {
        btn.disabled = !d.allow_write;
      });
      if (d.allow_write) {
        writeStatus.textContent = 'WRITE MODE ENABLED — Commands dispatch with STM32 CRC-32';
        writeStatus.style.color = '#f59e0b';
      } else {
        writeStatus.textContent = 'Read-only mode active (set BULLETSPY_ALLOW_WRITE=1 to enable controls)';
        writeStatus.style.color = '#9ca3af';
      }

      if (d.raw_frames && d.raw_frames.a0_temp) {
        document.getElementById('raw-hex').textContent =
          `0xA0 (TEMP/STATE): ${d.raw_frames.a0_temp}\\n` +
          `0xA1 (FAN/RPM)   : ${d.raw_frames.a1_fan}\\n` +
          `0xA2 (POWER/IGBT): ${d.raw_frames.a2_power}`;
      } else {
        document.getElementById('raw-hex').textContent =
          `PKT1: ${d.raw_pkt1_hex}\\nPKT2: ${d.raw_pkt2_hex}`;
      }

      history.push(d);
      if (history.length > MAX_POINTS) history.shift();
      drawChart();

      // Sample RoR every 5s (keep last 12 samples = 60s) & run Roast Monster every 10s
      const nowMs = Date.now();
      if (nowMs - lastRorSampleTime >= 5000) {
        lastRorSampleTime = nowMs;
        rorHistory.push(Number(d.bean_ror_c_min || 0));
        if (rorHistory.length > 12) rorHistory.shift();
      }
      evaluateRoastState(d, false);
    };
  }
  connect();
</script>
</body>
</html>
"""


@app.get("/", response_class=HTMLResponse)
async def dashboard() -> str:
    return DASHBOARD_HTML


def ensure_self_signed_cert(
    cert_path: Optional[str] = None,
    key_path: Optional[str] = None,
    cert_dir: Optional[str] = None,
) -> tuple[str, str]:
    """
    Ensures a local self-signed TLS certificate and private key exist so BulletSpy
    can serve over HTTPS (establishing a browser Secure Context `window.isSecureContext`
    on LAN for Chrome's Gemini Nano `LanguageModel` API, Web Speech, and Clipboard).
    """
    base_dir = Path(cert_dir) if cert_dir else Path(__file__).resolve().parent / ".certs"
    c_file = Path(cert_path) if cert_path else base_dir / "cert.pem"
    k_file = Path(key_path) if key_path else base_dir / "key.pem"

    if c_file.is_file() and k_file.is_file():
        return str(c_file), str(k_file)

    c_file.parent.mkdir(parents=True, exist_ok=True)
    k_file.parent.mkdir(parents=True, exist_ok=True)

    # Detect local hostname and LAN IP for Subject Alternative Name (SAN)
    hostname = socket.gethostname()
    lan_ip = "127.0.0.1"
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            lan_ip = s.getsockname()[0]
    except Exception:
        pass

    san_entries = ["DNS:localhost", "DNS:bulletspy", "DNS:bulletspy.local", "IP:127.0.0.1"]
    if hostname and hostname not in ("localhost", "bulletspy"):
        san_entries.append(f"DNS:{hostname}")
        san_entries.append(f"DNS:{hostname}.local")
    if lan_ip and lan_ip != "127.0.0.1":
        san_entries.append(f"IP:{lan_ip}")
    san_str = ",".join(san_entries)

    log.info("Generating self-signed HTTPS certificate in %s (SAN: %s)...", c_file.parent, san_str)
    cmd = [
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        str(k_file),
        "-out",
        str(c_file),
        "-days",
        "3650",
        "-subj",
        f"/CN={hostname or 'bulletspy'}/O=BulletSpy",
        "-addext",
        f"subjectAltName={san_str}",
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        os.chmod(k_file, 0o600)
    except Exception:
        pass
    return str(c_file), str(k_file)


def main() -> None:
    use_https_default = os.environ.get("BULLETSPY_HTTPS", "0").lower() in ("1", "true", "yes")
    parser = argparse.ArgumentParser(description="BulletSpy Aillio Bullet R1/R2 Telemetry Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="Bind port (default: 8080)")
    parser.add_argument(
        "--allow-write",
        action="store_true",
        default=ALLOW_WRITE,
        help="Enable USB command dispatch to the Bullet (default: read-only)",
    )
    parser.add_argument(
        "--https",
        action="store_true",
        default=use_https_default,
        help="Serve over HTTPS (and WSS) using an auto-generated self-signed certificate",
    )
    parser.add_argument("--ssl-certfile", default=None, help="Custom TLS certificate path (optional)")
    parser.add_argument("--ssl-keyfile", default=None, help="Custom TLS private key path (optional)")
    args = parser.parse_args()
    driver.allow_write = args.allow_write

    ssl_kwargs: Dict[str, Any] = {}
    if args.https or (args.ssl_certfile and args.ssl_keyfile):
        cert_file, key_file = ensure_self_signed_cert(args.ssl_certfile, args.ssl_keyfile)
        ssl_kwargs["ssl_certfile"] = cert_file
        ssl_kwargs["ssl_keyfile"] = key_file
        log.info("HTTPS enabled — open https://<host>:%d (WebSocket will auto-upgrade to wss://)", args.port)

    import uvicorn
    uvicorn.run(app, host=args.host, port=args.port, **ssl_kwargs)


if __name__ == "__main__":
    main()
