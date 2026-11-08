To answer your question first: **Wait—there actually *is* a command listener on the Bullet!**

While the roaster won't broadcast anything on its own (it sits silent until a USB host polls it), its USB OUT endpoint (`0x03`) accepts bidirectional control packets. That’s how RoasTime and Artisan trigger **PRS (Cycle/State advance)**, adjust **Induction Power (P0–P9)**, change **Exhaust Fan speed (F1–F12)**, and set **Drum speed (D1–D9)**.

For a safe, rock-solid POC, we can design **`BulletSpy`** primarily as a "read-only" active poller (sending only the benign status-request heartbeat so it never accidentally alters a roast), while building the command-dispatch hooks right into the architecture so you can flip them on whenever you want to drive the roaster from a script or model.

Here is the complete architecture and POC specification for **BulletSpy** running on one of your Raspberry Pi 4s.

---

# Project Spec: `BulletSpy` (v0.1 POC)

### 1. System Architecture

* **Target Hardware:** Raspberry Pi 4 (connected to Bullet R2 via USB-C to USB-A cable).
* **OS / Runtime:** Ubuntu / Debian Linux, Python 3.10+, `libusb-1.0`.
* **Daemon (`bulletspy.py`):**
* Claims the Bullet USB interface via `pyusb` (or reads its CDC/ACM serial stream, depending on whether the R2 enumerates in raw bulk or virtual COM mode).
* Runs an asynchronous 4Hz (250ms) polling loop that transmits the status-inquiry heartbeat and reads back the 128-byte telemetry frame.
* Unpacks the binary C-structs into a unified JSON state dictionary.
* Broadcasts the JSON payload to all connected clients via an unauthenticated **FastAPI WebSocket (`/ws`)** and serves a single-page **autoloading dark-mode dashboard (`/`)**.



---

### 2. USB Hardware & Protocol Layer

#### Device Enumeration

The Bullet uses an STM32 microcontroller under the hood (`VID: 0x0483`). Depending on the hardware revision and R2 firmware version, it presents one of these Product IDs:

* **`0x5741` / `0xa27e`:** Classic Aillio R1 / IBTS raw `libusb` bulk transfer interface.
* **`0xa4cd` (or R2 CDC/Bulk):** R2 interface (Note: Artisan split the R1 and R2 implementations into `artisanlib/aillio_r1.py` and `artisanlib/aillio_r2.py`—see the byte-offset tip below for grabbing exact R2 struct offsets).

#### Endpoints & Polling Cycle

* **OUT Endpoint (`0x03`):** Host-to-Bullet commands and status poll triggers.
* **IN Endpoint (`0x81`):** Bullet-to-Host 64-byte bulk packets.
* **The Heartbeat:** The Bullet will not stream unsolicited packets. Every 250ms, the Pi writes two 2-byte inquiry commands (`[0x30, 0x01]` and `[0x30, 0x02]`) to `0x03`, and immediately reads two 64-byte frames (128 bytes total) from `0x81`.

#### Roaster Cycle States (`state_id`)

The cycle byte in the telemetry packet maps directly to the roaster's state machine:

* `0x00`: **OFF** (Idle / Standby)
* `0x02`: **PH** (Pre-heating)
* `0x04`: **CHARGE** (Ready to drop green beans)
* `0x06`: **ROASTING** (Active roast cycle)
* `0x08`: **COOLING** (Beans dumped, cooling tray active)
* `0x09`: **SHUTDOWN**

#### Command Listener Opcodes (Optional Write Mode)

If you decide you *do* want `BulletSpy` to make the roaster do things later, sending these byte arrays to `0x03` triggers physical actions on the Bullet:

* **Advance Cycle (PRS Button):** `[0x30, 0x01, 0x00, 0x00]`
* **Heater Step Up / Down:** `[0x34, 0x01, 0xaa, 0xaa]` / `[0x34, 0x02, 0xaa, 0xaa]`
* **Fan Step Up / Down:** `[0x31, 0x01, 0xaa, 0xaa]` / `[0x31, 0x02, 0xaa, 0xaa]`
* **Drum Speed (1–9):** `[0x32, 0x01, <speed_int>, 0x00]`

---

### 3. Step-by-Step Pi 4 Setup

#### A. Non-Root USB Permissions (`udev`)

Create `/etc/udev/rules.d/99-aillio.rules` on the Pi 4 so your Python service doesn't need `sudo` to claim the USB port:

```udev
# Aillio Bullet R1 / R2 STM32 USB Rules
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="5741", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="a27e", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="a4cd", MODE="0666", GROUP="plugdev"
# Catch-all for any Aillio STM32 PID on the roaster cart Pi:
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", MODE="0666", GROUP="plugdev"

```

Reload `udev`:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger

```

#### B. Dependencies

```bash
sudo apt update && sudo apt install -y libusb-1.0-0-dev python3-venv
mkdir ~/bulletspy && cd ~/bulletspy
python3 -m venv venv
source venv/bin/activate
pip install pyusb fastapi uvicorn websockets

```

---

### 4. POC Code: `bulletspy.py` (Single-File Service + UI)

Save this as `bulletspy.py`. It includes a **Mock Mode fallback** if the Bullet is unplugged or powered off, so you can test the WebSocket stream and dashboard immediately on your Pi 4.

```python
import asyncio
import json
import math
import struct
import time
from typing import Set
import usb.core
import usb.util
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

# --- CONFIGURATION ---
AILLIO_VID = 0x0483
# Known PIDs across R1 / IBTS / R2 revisions; script auto-discovers any 0x0483 device if needed
KNOWN_PIDS = [0x5741, 0xA27E, 0xA4CD]
EP_OUT = 0x03
EP_IN = 0x81
POLL_INTERVAL = 0.25  # 4 Hz

STATE_MAP = {
    0x00: "OFF",
    0x02: "PREHEATING",
    0x04: "CHARGE",
    0x06: "ROASTING",
    0x08: "COOLING",
    0x09: "SHUTDOWN",
}

app = FastAPI(title="BulletSpy Telemetry Server")
clients: Set[WebSocket] = set()
latest_telemetry = {}

class BulletDriver:
    def __init__(self):
        self.dev = None
        self.mock_mode = False
        self.t0 = time.time()

    def connect(self):
        for pid in KNOWN_PIDS:
            self.dev = usb.core.find(idVendor=AILLIO_VID, idProduct=pid)
            if self.dev:
                break
        if not self.dev:
            # Fallback: find any STM32 device from Aillio
            self.dev = usb.core.find(idVendor=AILLIO_VID)

        if not self.dev:
            print("[WARN] No Aillio Bullet detected on USB. Running in SIMULATED MOCK MODE.")
            self.mock_mode = True
            return

        print(f"[INFO] Found Bullet: VID={hex(self.dev.idVendor)} PID={hex(self.dev.idProduct)}")
        try:
            if self.dev.is_kernel_driver_active(0):
                self.dev.detach_kernel_driver(0)
        except Exception:
            pass
        self.dev.set_configuration()
        usb.util.claim_interface(self.dev, 0)
        self.mock_mode = False

    def poll(self) -> dict:
        if self.mock_mode:
            return self._mock_Read()

        try:
            # Request Packet 1 & Packet 2
            self.dev.write(EP_OUT, [0x30, 0x01], timeout=200)
            pkt1 = bytes(self.dev.read(EP_IN, 64, timeout=200))
            self.dev.write(EP_OUT, [0x30, 0x02], timeout=200)
            pkt2 = bytes(self.dev.read(EP_IN, 64, timeout=200))
            return self._decode(pkt1, pkt2)
        except Exception as e:
            print(f"[ERROR] USB Read Error: {e}. Attempting reconnect...")
            self.connect()
            return self._mock_Read()

    def _decode(self, pkt1: bytes, pkt2: bytes) -> dict:
        """
        Unpacks the two 64-byte bulk frames into human-readable metrics.
        Note: We also include raw hex so you can verify R2-specific byte offsets.
        """
        try:
            # Standard Little-Endian float & uint8 unpacking
            # Offsets based on standard Aillio IBTS frame layout:
            bt_probe = round(struct.unpack("<f", pkt1[0:4])[0], 1)
            bt_ror = round(struct.unpack("<f", pkt1[4:8])[0], 1)
            dt_ibts = round(struct.unpack("<f", pkt1[8:12])[0], 1)
            dt_ror = round(struct.unpack("<f", pkt1[12:16])[0], 1)
            exhaust_temp = round(struct.unpack("<f", pkt1[16:20])[0], 1)
            
            state_byte = pkt1[29]
            heater_lvl = pkt1[26]
            fan_lvl = pkt1[27]
            drum_lvl = pkt1[28]
            voltage = round(struct.unpack("<H", pkt1[32:34])[0] / 10.0, 1)
            fan_rpm = struct.unpack("<H", pkt1[36:38])[0]
        except Exception:
            bt_probe = bt_ror = dt_ibts = dt_ror = exhaust_temp = voltage = 0.0
            state_byte = heater_lvl = fan_lvl = drum_lvl = fan_rpm = 0

        return {
            "timestamp": round(time.time(), 3),
            "mode": "LIVE_USB",
            "cycle_id": state_byte,
            "cycle": STATE_MAP.get(state_byte, f"UNKNOWN (0x{state_byte:02x})"),
            "bean_temp_c": bt_probe,
            "bean_ror_c_min": bt_ror,
            "drum_temp_ibts_c": dt_ibts,
            "drum_ror_c_min": dt_ror,
            "exhaust_temp_c": exhaust_temp,
            "heater_power": heater_lvl,
            "fan_speed": fan_lvl,
            "drum_speed": drum_lvl,
            "fan_rpm": fan_rpm,
            "voltage_v": voltage,
            "raw_pkt1_hex": pkt1.hex(),
            "raw_pkt2_hex": pkt2.hex(),
        }

    def _mock_Read(self) -> dict:
        elapsed = time.time() - self.t0
        bt = round(165.0 + 35.0 * math.sin(elapsed / 30.0), 1)
        dt = round(bt + 22.4 + 3.0 * math.cos(elapsed / 10.0), 1)
        ror = round(12.5 * math.cos(elapsed / 30.0), 1)
        return {
            "timestamp": round(time.time(), 3),
            "mode": "SIMULATED",
            "cycle_id": 0x06,
            "cycle": "ROASTING",
            "bean_temp_c": bt,
            "bean_ror_c_min": ror,
            "drum_temp_ibts_c": dt,
            "drum_ror_c_min": round(ror + 1.2, 1),
            "exhaust_temp_c": round(bt - 45.0, 1),
            "heater_power": 7,
            "fan_speed": 4,
            "drum_speed": 9,
            "fan_rpm": 1420,
            "voltage_v": 238.4,
            "raw_pkt1_hex": "00" * 64,
            "raw_pkt2_hex": "00" * 64,
        }

driver = BulletDriver()

async def telemetry_loop():
    global latest_telemetry
    driver.connect()
    while True:
        latest_telemetry = driver.poll()
        if clients:
            payload = json.dumps(latest_telemetry)
            dead_clients = set()
            for ws in clients:
                try:
                    await ws.send_text(payload)
                except Exception:
                    dead_clients.add(ws)
            clients.difference_update(dead_clients)
        await asyncio.sleep(POLL_INTERVAL)

@app.on_event("startup")
async def startup_event():
    asyncio.create_task(telemetry_loop())

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    clients.add(websocket)
    try:
        while True:
            # Keep alive; ignore incoming unless we enable command listener later
            await websocket.receive_text()
    except WebSocketDisconnect:
        clients.discard(websocket)

@app.get("/api/state")
async def get_state():
    return latest_telemetry

@app.get("/", response_class=HTMLResponse)
async def dashboard():
    return """
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>BulletSpy // Live Telemetry</title>
<style>
  :root { --bg: #0f1115; --card: #181b22; --text: #e5e7eb; --muted: #9ca3af; --accent: #f59e0b; --ibts: #ef4444; --bt: #3b82f6; --ok: #10b981; }
  body { background: var(--bg); color: var(--text); font-family: 'JetBrains Mono', monospace, sans-serif; margin: 0; padding: 24px; }
  .header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #2d3139; padding-bottom: 16px; margin-bottom: 24px; }
  .badge { padding: 6px 14px; border-radius: 6px; font-weight: bold; background: #262936; color: var(--accent); font-size: 1.2rem; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 16px; }
  .card { background: var(--card); padding: 20px; border-radius: 10px; border: 1px solid #262936; }
  .label { color: var(--muted); font-size: 0.85rem; text-transform: uppercase; letter-spacing: 0.08em; }
  .val { font-size: 2.6rem; font-weight: 700; margin-top: 8px; }
  .sub { font-size: 0.95rem; color: var(--muted); margin-top: 4px; }
  .bt { color: var(--bt); }
  .ibts { color: var(--ibts); }
  pre { background: #090a0d; padding: 12px; border-radius: 8px; overflow-x: auto; font-size: 0.75rem; color: #6b7280; }
</style>
</head>
<body>
  <div class="header">
    <div>
      <h1 style="margin:0; font-size:1.5rem;">BULLETSPY // R2 TELEMETRY</h1>
      <small id="conn-status" style="color:var(--ok);">Connecting WebSocket...</small>
    </div>
    <div id="cycle-badge" class="badge">WAITING</div>
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
      <div id="volts" class="sub">Line: ---.- V</div>
    </div>
    <div class="card">
      <div class="label">Exhaust Fan / Drum</div>
      <div id="fan" class="val">F- / D-</div>
      <div id="rpm" class="sub">Blower: ---- RPM</div>
    </div>
    <div class="card">
      <div class="label">Exhaust Temp</div>
      <div id="exhaust" class="val">--.- °C</div>
      <div id="mode" class="sub">Source: --</div>
    </div>
  </div>

  <h3 style="margin-top:32px; color:var(--muted); font-size:0.9rem;">RAW USB PAYLOAD INSPECTOR (64B x 2)</h3>
  <pre id="raw-hex">Waiting for frames...</pre>

<script>
  function connect() {
    const ws = new WebSocket(`ws://${location.host}/ws`);
    const status = document.getElementById('conn-status');
    ws.onopen = () => { status.textContent = 'LIVE STREAM CONNECTED (4Hz)'; status.style.color = '#10b981'; };
    ws.onclose = () => { status.textContent = 'DISCONNECTED - RETRYING...'; status.style.color = '#ef4444'; setTimeout(connect, 2000); };
    ws.onmessage = (event) => {
      const d = JSON.parse(event.data);
      document.getElementById('cycle-badge').textContent = d.cycle;
      document.getElementById('dt').textContent = `${d.drum_temp_ibts_c.toFixed(1)} °C`;
      document.getElementById('dt-ror').textContent = `RoR: ${d.drum_ror_c_min.toFixed(1)} °C/m`;
      document.getElementById('bt').textContent = `${d.bean_temp_c.toFixed(1)} °C`;
      document.getElementById('bt-ror').textContent = `RoR: ${d.bean_ror_c_min.toFixed(1)} °C/m`;
      document.getElementById('power').textContent = `P${d.heater_power}`;
      document.getElementById('volts').textContent = `Line: ${d.voltage_v.toFixed(1)} V`;
      document.getElementById('fan').textContent = `F${d.fan_speed} / D${d.drum_speed}`;
      document.getElementById('rpm').textContent = `Blower: ${d.fan_rpm} RPM`;
      document.getElementById('exhaust').textContent = `${d.exhaust_temp_c.toFixed(1)} °C`;
      document.getElementById('mode').textContent = `Source: ${d.mode}`;
      document.getElementById('raw-hex').textContent = `PKT1: ${d.raw_pkt1_hex}\\nPKT2: ${d.raw_pkt2_hex}`;
    };
  }
  connect();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)

```

---

### 5. Pro-Tip for Verifying the R2 Byte Offsets

Because Aillio tweaked the packet framing between the R1 V2 and the R2 (and Artisan maintains a dedicated `aillio_r2.py` driver specifically for the R2's frame structure), do a quick sanity check when you plug your Pi 4 into the Bullet R2:

1. Run `lsusb -v -d 0483:` on the Pi 4 with the Bullet plugged in. This will show you the exact `idProduct` and whether the R2 presents as a raw Vendor-Specific Bulk class (`0xFF`) or a CDC Serial Data interface (`0x0A`, which mounts as `/dev/ttyACM0`).
2. Pull the exact struct offsets from Artisan's R2 driver right onto your Pi for reference:
```bash
curl -sL https://raw.githubusercontent.com/artisan-roaster-scope/artisan/master/src/artisanlib/aillio_r2.py -o aillio_r2_ref.py

```


3. Compare the `struct.unpack` offsets in `aillio_r2_ref.py` against the `RAW USB PAYLOAD INSPECTOR` box at the bottom of your `BulletSpy` web page—if any byte index shifted on your R2 firmware, you can adjust the slice indices in `_decode()` in ten seconds.
