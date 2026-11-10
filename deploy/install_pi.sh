#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Shield-1.0.0
# Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
# Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
set -euo pipefail

# Turnkey installer for BulletSpy on Raspberry Pi 4 (Ubuntu / Debian)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "[1/4] Installing system dependencies (libusb-1.0, python3-venv)..."
sudo apt-get update
sudo apt-get install -y libusb-1.0-0-dev python3-venv

echo "[2/4] Installing udev rules for Aillio Bullet R1/R2 (VID 0x0483)..."
sudo cp "${SCRIPT_DIR}/99-aillio.rules" /etc/udev/rules.d/99-aillio.rules
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "[3/4] Setting up Python virtual environment in ${PROJECT_DIR}/venv..."
if [ ! -d "${PROJECT_DIR}/venv" ]; then
    python3 -m venv "${PROJECT_DIR}/venv"
fi
"${PROJECT_DIR}/venv/bin/pip" install --upgrade pip
"${PROJECT_DIR}/venv/bin/pip" install -r "${PROJECT_DIR}/requirements.txt"

echo "[4/4] Setup complete!"
echo "To run manually:"
echo "  ${PROJECT_DIR}/venv/bin/python ${PROJECT_DIR}/bulletspy.py"
echo "To check connected Aillio USB devices:"
echo "  lsusb -v -d 0483:"
