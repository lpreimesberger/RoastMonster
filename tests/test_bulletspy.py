# SPDX-License-Identifier: PolyForm-Shield-1.0.0
# Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
# Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.

import struct
from fastapi.testclient import TestClient

from bulletspy import (
    BulletDriver,
    CMD_HEATER_INCR,
    CMD_PRS,
    FRAME_SIZE,
    app,
    calculate_stm32_crc32,
    compute_roast_stage,
    ensure_self_signed_cert,
    prepare_r2_command,
)


def test_ensure_self_signed_cert(tmp_path):
    cert_file, key_file = ensure_self_signed_cert(cert_dir=str(tmp_path))
    assert (tmp_path / "cert.pem").is_file()
    assert (tmp_path / "key.pem").is_file()
    assert "BEGIN CERTIFICATE" in (tmp_path / "cert.pem").read_text()
    # Second call reuses existing files
    cert_file2, key_file2 = ensure_self_signed_cert(cert_dir=str(tmp_path))
    assert cert_file == cert_file2 and key_file == key_file2


def test_compute_roast_stage_zones_and_cycles():
    assert compute_roast_stage(200.0, 0x04)["roast_stage_short"] == "CHARGE READY"
    assert compute_roast_stage(135.0, 0x06)["roast_stage_short"] == "DRYING"
    assert compute_roast_stage(162.0, 0x06)["roast_stage_short"] == "MAILLARD"
    assert compute_roast_stage(185.0, 0x06)["roast_stage_short"] == "PRE-1ST CRACK"
    assert compute_roast_stage(199.5, 0x06)["roast_stage_short"] == "FIRST CRACK"
    assert compute_roast_stage(210.0, 0x06)["roast_stage_short"] == "CITY"
    assert compute_roast_stage(217.0, 0x06)["roast_stage_short"] == "FULL CITY"
    assert compute_roast_stage(223.0, 0x06)["roast_stage_short"] == "VIENNA"
    assert compute_roast_stage(229.0, 0x06)["roast_stage_short"] == "FRENCH"
    assert compute_roast_stage(235.0, 0x06)["roast_stage_short"] == "ITALIAN / DROP!"


def test_stm32_crc32_and_prepare_r2_command():
    pkt = prepare_r2_command(CMD_PRS)
    assert len(pkt) == 8
    assert list(pkt[:4]) == CMD_PRS
    expected_crc = calculate_stm32_crc32(pkt)
    actual_crc = int.from_bytes(pkt[-4:], "little")
    assert actual_crc == expected_crc


def test_r2_binary_frame_unpacking():
    drv = BulletDriver(allow_write=False)
    a0, a1, a2 = drv._build_mock_r2_frames()
    assert len(a0) == FRAME_SIZE and a0[0] == 0xA0
    assert len(a1) == FRAME_SIZE and a1[0] == 0xA1
    assert len(a2) == FRAME_SIZE and a2[0] == 0xA2

    drv.decode_r2_frame(a0)
    drv.decode_r2_frame(a1)
    state = drv.decode_r2_frame(a2)

    assert state["drum_temp_ibts_c"] > 100.0
    assert state["bean_temp_c"] > 100.0
    assert state["voltage_v"] > 200.0
    assert state["fan_rpm"] > 1000
    assert state["drum_rpm"] > 40
    assert len(state["raw_frames"]["a0_temp"]) == 128


def test_r1_binary_packet_unpacking():
    drv = BulletDriver(allow_write=False)
    pkt1 = bytearray(FRAME_SIZE)
    pkt2 = bytearray(FRAME_SIZE)
    struct.pack_into("<f", pkt1, 0, 210.4)   # IBTS drum temp
    struct.pack_into("<f", pkt1, 4, 14.2)    # IBTS RoR
    struct.pack_into("<f", pkt1, 8, 188.6)   # Bean probe
    struct.pack_into("<f", pkt1, 12, 11.8)   # Bean RoR
    struct.pack_into("<f", pkt1, 16, 145.0)  # Exhaust temp
    pkt1[24] = 6   # minutes
    pkt1[25] = 42  # seconds
    pkt1[26] = 5   # fan
    pkt1[27] = 8   # heater
    pkt1[28] = 9   # drum
    pkt1[29] = 0x06  # ROASTING
    struct.pack_into("<h", pkt1, 44, 1650)   # fan RPM
    struct.pack_into("<h", pkt1, 48, 239)    # voltage

    res = drv.decode_r1_packets(bytes(pkt1), bytes(pkt2), mode="LIVE_USB")
    assert res["cycle"] == "ROASTING"
    assert res["drum_temp_ibts_c"] == 210.4
    assert res["bean_temp_c"] == 188.6
    assert res["heater_power"] == 8
    assert res["fan_speed"] == 5
    assert res["drum_speed"] == 9
    assert res["roast_clock"] == "06:42"
    assert res["voltage_v"] == 239.0


def test_command_dispatch_read_only_guard_and_write_mode():
    drv = BulletDriver(allow_write=False)
    try:
        drv.dispatch_command("prs")
        assert False, "Expected PermissionError when allow_write is False"
    except PermissionError:
        pass

    drv.allow_write = True
    res = drv.dispatch_command("heater", 9)
    assert res["ok"] is True
    assert drv.state["heater_power"] == 9
    assert len(res["queued_packets_hex"]) > 0
    # Verify queued packet starts with CMD_HEATER_INCR and has 4-byte CRC-32 (8 bytes = 16 hex chars)
    assert res["queued_packets_hex"][0].startswith(bytes(CMD_HEATER_INCR).hex())
    assert len(res["queued_packets_hex"][0]) == 16


def test_fastapi_endpoints_and_websocket():
    with TestClient(app) as client:
        # Dashboard HTML
        resp = client.get("/")
        assert resp.status_code == 200
        assert "BULLETSPY // R2 TELEMETRY" in resp.text

        # Telemetry JSON API
        state_resp = client.get("/api/state")
        assert state_resp.status_code == 200
        data = state_resp.json()
        assert data["mode"] == "SIMULATED"
        assert data["cycle"] == "ROASTING"
        assert "drum_temp_ibts_c" in data

        # Command endpoint should return 403 in default read-only mode
        cmd_resp = client.post("/api/command", json={"action": "prs"})
        assert cmd_resp.status_code == 403

        # WebSocket initial frame
        with client.websocket_connect("/ws") as ws:
            msg = ws.receive_json()
            assert msg["mode"] == "SIMULATED"
            assert "raw_pkt1_hex" in msg
