#!/bin/bash
# Downloads the ANT SoftDevices from Garmin (developer.garmin.com/ant-program/nrf5-sdk/)
# and unpacks S340 v7.0.1 for nRF52840 into garmin/s340_nrf52_7.0.1/.
#
# The SoftDevices are free for non-commercial evaluation; downloading them means accepting
# the ANT+ Adopter Agreement and the ANT SoftDevice license (License_Agreement_ANT_Softdevice_rev3_3.pdf).
# They are NOT redistributable, which is why they are not part of this repository.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/garmin"
URL="https://developer.garmin.com/downloads/ant/SoftDevices_Current_Items.zip"

if [ "${1:-}" != "--accept-license" ]; then
  cat <<EOF
This downloads ANT SoftDevices from Garmin:
  $URL
They are licensed for NON-COMMERCIAL evaluation only (commercial use needs a paid license key):
  https://developer.garmin.com/ant-program/licensing/adopter-agreement/
EOF
  read -r -p "Accept the ANT+ Adopter Agreement and download? [y/N] " answer
  [[ "$answer" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
fi

mkdir -p "$DEST"
cd "$DEST"
curl -fL --progress-bar -o SoftDevices_Current_Items.zip "$URL"
rm -rf SoftDevices_Current_Items
unzip -q -o SoftDevices_Current_Items.zip
ZIP=$(find . -name "ANT_s340_nrf52_7.0.1.zip" | head -1)
[ -n "$ZIP" ] || { echo "ANT_s340_nrf52_7.0.1.zip not found in the archive" >&2; exit 1; }
rm -rf s340_nrf52_7.0.1
unzip -q -o "$ZIP" -d s340_nrf52_7.0.1

echo "S340 v7.0.1 unpacked to $DEST/s340_nrf52_7.0.1"
ls "$DEST/s340_nrf52_7.0.1"
