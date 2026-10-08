#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SERIAL=${1:-}
ADB=adb
if [ -n "$SERIAL" ]; then
  ADB="adb -s $SERIAL"
fi

# shellcheck disable=SC2086
$ADB get-state >/dev/null
# shellcheck disable=SC2086
$ADB shell 'mkdir -p /userdata/dual-person-demo'
# shellcheck disable=SC2086
$ADB push "$ROOT_DIR/runtime/." /userdata/dual-person-demo/
# The stock IMX415 IQ file only contains 3840x2160 LSC data. Without the
# validated 640x640 entry, RAW ROI frames are valid but RKISP emits black NV12.
# Keep a recoverable copy before installing the ROI-capable calibration.
# shellcheck disable=SC2086
$ADB shell 'test -e /etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json.roi-backup || cp /etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json /etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json.roi-backup'
# shellcheck disable=SC2086
$ADB push "$ROOT_DIR/runtime/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json" /etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json
# shellcheck disable=SC2086
$ADB shell 'chmod 644 /etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json; sync'
# shellcheck disable=SC2086
$ADB shell 'chmod +x /userdata/dual-person-demo/*.sh /userdata/dual-person-demo/imx415_dual_person_demo'
echo 'Installed at /userdata/dual-person-demo'
echo "Start with: $ADB shell 'cd /userdata/dual-person-demo && ./run.sh'"
