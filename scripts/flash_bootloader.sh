#!/bin/bash
# Builds the Adafruit nRF52 UF2 bootloader for nice!nano with the ANT SoftDevice S340 v7.0.1
# and flashes "bootloader + SoftDevice" over USB serial DFU (no SWD programmer needed).
#
# Usage: scripts/flash_bootloader.sh [serial port]     (board must be in bootloader mode: NICENANO drive)
#        scripts/flash_bootloader.sh --build-only
#
# Needs: arm-none-eabi-gcc, git, python3, S340 in garmin/ (scripts/get_softdevice.sh).
# Env: ANT_LICENSE_KEY (default: evaluation key from nrf_sdm.h), BOOTLOADER_REF (git ref to build).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S340_DIR="${S340_DIR:-$ROOT/garmin/s340_nrf52_7.0.1}"
WORK="$ROOT/build/bootloader"
BL="$WORK/Adafruit_nRF52_Bootloader"
VENV="$WORK/venv"
BOOTLOADER_REF="${BOOTLOADER_REF:-c67f0bc}"   # tested: 0.11.0 (May 2026)

[ -f "$S340_DIR/ANT_s340_nrf52_7.0.1.hex" ] || { echo "S340 not found in $S340_DIR (run scripts/get_softdevice.sh)" >&2; exit 1; }
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not found (brew install --cask gcc-arm-embedded)" >&2; exit 1; }

if [ -z "${ANT_LICENSE_KEY:-}" ]; then
  ANT_LICENSE_KEY=$(sed -nE 's|^//#define ANT_LICENSE_KEY "([0-9a-fA-F-]+)".*|\1|p' "$S340_DIR/ANT_s340_nrf52_7.0.1.API/include/nrf_sdm.h" | head -1)
  echo "Using the ANT EVALUATION license key (non-commercial use only)"
fi

mkdir -p "$WORK"
if [ ! -d "$BL/.git" ]; then
  git clone https://github.com/adafruit/Adafruit_nRF52_Bootloader.git "$BL"
fi
git -C "$BL" fetch --tags -q origin
git -C "$BL" checkout -q "$BOOTLOADER_REF"
git -C "$BL" submodule update --init --depth 1 lib/tinyusb lib/nrfx lib/uf2

# Place S340 files the way lib/softdevice/s340_nrf52_7.0.1/readme.md describes
D="$BL/lib/softdevice/s340_nrf52_7.0.1"
rm -rf "$D/s340_nrf52_7.0.1_API"
cp -R "$S340_DIR/ANT_s340_nrf52_7.0.1.API" "$D/s340_nrf52_7.0.1_API"
cp "$S340_DIR/ANT_s340_nrf52_7.0.1.hex" "$D/s340_nrf52_7.0.1_softdevice.hex"

[ -x "$VENV/bin/adafruit-nrfutil" ] || { python3 -m venv "$VENV" && "$VENV/bin/pip" -q install adafruit-nrfutil intelhex; }
export PATH="$VENV/bin:$PATH"

# Version is passed explicitly: the Makefile parses `git describe` with GNU sed syntax that BSD sed rejects.
VER=$(git -C "$BL" describe --tags --abbrev=0 | sed -E 's/^v//; s/\./ /g')
make -C "$BL" BOARD=nice_nano SD_NAME=s340 SD_VERSION=7.0.1 ANT_LICENSE_KEY="$ANT_LICENSE_KEY" "_VER3=$VER" all
ZIP=$(ls "$BL"/_build/build-nice_nano/nice_nano_bootloader-*_s340_7.0.1.zip | head -1)
echo "Built: $ZIP"

[ "${1:-}" = "--build-only" ] && exit 0

PORT="${1:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "No serial port found; put the board in bootloader mode (double tap reset)" >&2; exit 1; }
cat <<EOF

About to replace the SoftDevice and bootloader on $PORT with S340 v7.0.1 + $(basename "$ZIP").
The current application will be erased. Do NOT unplug the board during the update.
EOF
read -r -p "Continue? [y/N] " answer
[[ "$answer" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
adafruit-nrfutil --verbose dfu serial --package "$ZIP" -p "$PORT" -b 115200 --singlebank
echo "Done. INFO_UF2.TXT on the NICENANO drive should now say: SoftDevice: S340 7.0.1"
