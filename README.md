# BulletSpy

Asynchronous **4Hz USB Telemetry Server, Binary Protocol Unpacker, and Live Dark-Mode Dashboard** for the **Aillio Bullet R2** (`0xa4cd`) and **Bullet R1 / IBTS** (`0x5741`, `0xa27e`).

Designed to run lightweight on a **Raspberry Pi 4** bolted to your roaster cart while offloading rich UI visualization, audio synthesis, and optional on-device AI analysis to any browser on your LAN.

---

## Key Features

- **Dual-Protocol USB Driver (`R2` & `R1`):**
  - **Bullet R2 (`0xa4cd`, Protocol V2):** Unpacks multiplexed 64-byte USB bulk frames (`0xA0` Temperature/State, `0xA1` Fan/Drum RPMs/Errors, `0xA2` Active Power/Voltage/Current/IGBT Temps) on USB Interface `0x01`.
  - **Bullet R1 / IBTS (`0x5741` / `0xa27e`, Protocol V1):** Polls dual 64-byte status packets (`[0x30, 0x01]` & `[0x30, 0x03]`).
  - **Binary Simulated Mock Mode:** Automatically activates when no roaster is plugged into USB—synthesizing real binary `0xA0`/`0xA1`/`0xA2` frames with valid STM32 CRC-32 checksums and feeding them through the live binary parser.
- **Safe Read-Only Polling by Default (Optional Write Mode):**
  - Runs strictly read-only out of the box so it can never accidentally alter a roast.
  - Pass `--allow-write` (or `BULLETSPY_ALLOW_WRITE=1`) to unlock bidirectional control (`PRS` cycle advance, `Heater P0–P14`, `Fan F1–F12`, `Drum D1–D9`, `Preheat Setpoint`) with hardware **STM32 CRC-32** packet signing.
- **Bean Roast Level & Chemical Stage Tracker:**
  - Live 7-zone stage bar and color-coded phase readout across **Drying (`<150°C`)**, **Yellowing / Maillard (`150–175°C`)**, **Caramelization / Pre-FC (`175–196°C`)**, **First Crack / Cinnamon (`196–205°C`)**, **City (`205–214°C`)**, **Full City (`214–220°C`)**, and **Vienna / French / Dark (`220°C+`)**.
- **Target Drop Temp & Zero-Dependency Web Audio Chimes:**
  - 1-click roast degree presets or custom drop temperature (`°C`) tracking either **Bean Probe (`BT`)** or **Drum IBTS (`DT`)**, plotted as a dashed reference line on the rolling Canvas chart.
  - Synthesized Web Audio API multi-tone chimes and visual banners for **Preheat Complete (`CHARGE`)**, **First Crack**, **Approaching Target (`≤2.5°C`)**, and **Target Reached (Drop Alarm)**.
- **Roast Monster // Curve Anomaly Radar:**
  - Evaluates the last 30 seconds of Rate-of-Rise (`RoR`) slope every 10 seconds to warn of **RoR Crashes**, **Late RoR Flicks**, **Pre-Crack Heat Stalls**, and **Excessive IBTS Surface Temps**.
  - Works out of the box via a built-in deterministic **Roast Physics Engine**, and upgrades in 1 click to **Chrome Built-in AI (Gemini Nano / `LanguageModel` API)** using zero-leak session cloning (`baseSession.clone()`) and JSON Schema `responseConstraint`.
- **Hands-Free Voice Logger & Markdown Batch Exporter:**
  - Log spoken sensory notes (`🎤 Voice Note` via Web Speech API) stamped with exact roast clock, `BT`, `IBTS`, `RoR`, and stage, trigger spoken temperature callouts (`🗣️ Callout Temp`), and copy a full Markdown batch log (`📋 Copy Batch MD`) in one click.

---

## Quick Start on Raspberry Pi 4

Clone the repo onto your Pi 4 and run the turnkey installer in [`deploy/install_pi.sh`](deploy/install_pi.sh):

```bash
git clone <your-repo-url> ~/bulletspy
cd ~/bulletspy
./deploy/install_pi.sh
```

What [`deploy/install_pi.sh`](deploy/install_pi.sh) sets up:
1. Installs `libusb-1.0-0-dev` and `python3-venv`.
2. Installs [`deploy/99-aillio.rules`](deploy/99-aillio.rules) into `/etc/udev/rules.d/` and reloads `udevadm` so `bulletspy.py` can claim the STM32 USB interface (`VID 0x0483`) without `root`/`sudo`.
3. Creates `./venv` and installs dependencies from [`requirements.txt`](requirements.txt).

### Run Manually

```bash
# Safe Read-Only Telemetry Mode (default, port 8080):
./venv/bin/python bulletspy.py

# Custom port + HTTPS (auto-generates self-signed cert in .certs/) + Enable USB Command Dispatch:
./venv/bin/python bulletspy.py --port 9999 --https --allow-write
```

### Optional: Enable Auto-Start on Boot (`systemd`)

```bash
sudo cp deploy/bulletspy.service /etc/systemd/system/bulletspy@.service
sudo systemctl daemon-reload
sudo systemctl enable --now bulletspy@${USER}.service
sudo systemctl status bulletspy@${USER}.service
```

---

## Staging & Testing on x64 (No Roaster Required)

When no Aillio Bullet is detected on USB, [`bulletspy.py`](bulletspy.py) automatically boots into **Simulated Mock Mode**, generating synthetic 64-byte `0xA0`/`0xA1`/`0xA2` R2 binary frames:

```bash
python3 -m venv venv
./venv/bin/pip install -r requirements.txt

# Run the unit & integration test suite:
./venv/bin/python -m pytest -v

# Start the local server with write controls enabled:
./venv/bin/python bulletspy.py --port 9999 --allow-write
```

Open `http://localhost:9999` in your browser.

---

## Enabling Chrome Built-in Gemini Nano for Roast Monster (Optional)

**Roast Monster** runs its deterministic Roast Physics Engine automatically in all browsers. To upgrade it to Chrome's on-device **Gemini Nano (`LanguageModel` API)** on your laptop/desktop:

1. In Chrome (`v138+`), enable:
   - `chrome://flags/#optimization-guide-on-device-model` → **Enabled BypassPerfRequirement**
   - `chrome://flags/#prompt-api-for-gemini-nano` → **Enabled**
2. Because Chrome restricts `LanguageModel`, Web Speech, and Clipboard APIs to Secure Contexts (`HTTPS` or `localhost`), simply pass **`--https`** when starting `bulletspy.py`:
   ```bash
   ./venv/bin/python bulletspy.py --port 9999 --https --allow-write
   ```
   - On first run, `bulletspy.py` automatically generates a 10-year self-signed TLS certificate in `.certs/cert.pem` and `.certs/key.pem` (including your LAN IP and hostname in the Subject Alternative Name).
   - Open `https://<pi-ip>:9999` in Chrome, click **Advanced → Proceed** once, and `window.isSecureContext` will be `true` (and the WebSocket automatically upgrades to `wss://`) without weakening any browser security flags on your workstation!
3. Click **`✨ Enable Gemini Nano`** in the Roast Monster header on the dashboard.

---

## HTTP & WebSocket Endpoints

| Endpoint | Method | Description |
| :--- | :--- | :--- |
| `/` | `GET` | Autoloading dark-mode live telemetry dashboard |
| `/ws` | `WS` | 4Hz (250ms) JSON telemetry broadcast stream |
| `/api/state` | `GET` | Latest unpacked telemetry snapshot (JSON) |
| `/api/command` | `POST` | Dispatches control opcodes when `--allow-write` is enabled (`{"action": "prs" \| "heater_up" \| "heater_down" \| "fan_up" \| "fan_down" \| "heater" \| "fan" \| "drum" \| "preheat", "value": ...}`) |

---

## Repository Layout

- [`bulletspy.py`](bulletspy.py) — Single-file USB driver (`R1`/`R2`/Mock), binary frame unpacker, STM32 CRC-32 generator, FastAPI server, and embedded web dashboard.
- [`deploy/99-aillio.rules`](deploy/99-aillio.rules) — Linux `udev` rules for non-root USB access to `VID 0x0483`.
- [`deploy/bulletspy.service`](deploy/bulletspy.service) — `systemd` unit template for Raspberry Pi 4.
- [`deploy/install_pi.sh`](deploy/install_pi.sh) — Turnkey Raspberry Pi 4 installer script.
- [`tests/test_bulletspy.py`](tests/test_bulletspy.py) — `pytest` suite covering R2/R1 binary struct unpacking, STM32 CRC-32, roast stage classification, read-only guard, and WebSocket streaming.
- [`reference/`](reference/) — Reference Artisan R1 and R2 drivers (`aillio_r1_ref.py`, `aillio_r2_ref.py`) for byte-offset verification.
- [`spec.md`](spec.md) — Initial architecture & protocol specification.
