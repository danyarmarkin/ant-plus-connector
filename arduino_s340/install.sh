#!/bin/bash
# Adds the "nice!nano (S340 ANT+BLE)" board to the installed Adafruit nRF52 Arduino core.
# Re-run after every update of the Adafruit core (board manager overwrites the core folder).
#
# Env:
#   S340_DIR         unpacked S340 v7.0.1 (default: garmin/s340_nrf52_7.0.1, see scripts/get_softdevice.sh)
#   ANT_LICENSE_KEY  commercial ANT license key (default: evaluation key from the S340 nrf_sdm.h)
#   ARDUINO_DATA     Arduino data dir (default: ~/Library/Arduino15 on macOS, ~/.arduino15 on Linux)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
S340_DIR="${S340_DIR:-$HERE/../garmin/s340_nrf52_7.0.1}"
SD_API="$S340_DIR/ANT_s340_nrf52_7.0.1.API"

if [ -z "${ARDUINO_DATA:-}" ]; then
  if [ "$(uname)" = "Darwin" ]; then ARDUINO_DATA="$HOME/Library/Arduino15"; else ARDUINO_DATA="$HOME/.arduino15"; fi
fi
CORE_ROOT="$ARDUINO_DATA/packages/adafruit/hardware/nrf52"
[ -d "$CORE_ROOT" ] || { echo "Adafruit nRF52 core not found in $CORE_ROOT (install it via Boards Manager)" >&2; exit 1; }
CORE="$CORE_ROOT/$(ls "$CORE_ROOT" | sort -V | tail -1)"

[ -d "$SD_API/include" ] || { echo "S340 API not found: $SD_API (run scripts/get_softdevice.sh)" >&2; exit 1; }

if [ -z "${ANT_LICENSE_KEY:-}" ]; then
  # nrf_sdm.h ships the evaluation key commented out: //#define ANT_LICENSE_KEY "xxxx-...."
  ANT_LICENSE_KEY=$(sed -nE 's|^//#define ANT_LICENSE_KEY "([0-9a-fA-F-]+)".*|\1|p' "$SD_API/include/nrf_sdm.h" | head -1)
  [ -n "$ANT_LICENSE_KEY" ] || { echo "Evaluation key not found in nrf_sdm.h, set ANT_LICENSE_KEY" >&2; exit 1; }
  echo "Using the ANT EVALUATION license key (non-commercial use only)"
fi
echo "Adafruit core: $CORE"

rm -rf "$CORE/cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API"
cp -R "$SD_API" "$CORE/cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API"
cp "$HERE/nrf52840_s340_v7.ld" "$CORE/cores/nRF5/linker/"
rm -rf "$CORE/variants/nicenano_s340"
cp -R "$HERE/variant_nicenano_s340" "$CORE/variants/nicenano_s340"
sed "s|@ANT_LICENSE_KEY@|$ANT_LICENSE_KEY|" "$HERE/boards.local.txt" > "$CORE/boards.local.txt"

echo "Done. Restart Arduino IDE and pick: Tools > Board > Adafruit nRF52 > nice!nano (S340 ANT+BLE)"
