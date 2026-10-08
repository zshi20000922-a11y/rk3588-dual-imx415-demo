#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
failed=0

check_command() {
  if command -v "$1" >/dev/null 2>&1; then
    echo "[OK] command: $1"
  else
    echo "[FAIL] missing command: $1"
    failed=1
  fi
}

check_path() {
  if [ -e "$1" ]; then
    echo "[OK] path: $1"
  else
    echo "[FAIL] missing path: $1"
    failed=1
  fi
}

[ "$(uname -m)" = aarch64 ] || { echo "[FAIL] expected aarch64, got $(uname -m)"; failed=1; }
grep -q RK3588 /proc/device-tree/compatible 2>/dev/null || echo '[WARN] RK3588 was not confirmed from device tree'

for command_name in v4l2-ctl media-ctl python3 killall; do
  check_command "$command_name"
done
if [ -e /dev/rknpu ] || [ -e /dev/dri/renderD129 ]; then
  echo '[OK] NPU device'
else
  echo '[FAIL] missing NPU device (/dev/rknpu or /dev/dri/renderD129)'
  failed=1
fi
for device_path in /dev/media4 /dev/media5 /dev/v4l-subdev2 /dev/v4l-subdev7 /dev/video44 /dev/video53; do
  check_path "$device_path"
done
for file_path in "$APP_DIR/imx415_dual_person_demo" "$APP_DIR/model/yolov5n.rknn" "$APP_DIR/model/coco_80_labels_list.txt" "$APP_DIR/lib/librknnrt.so" "$APP_DIR/lib/librga.so"; do
  check_path "$file_path"
done

if ! python3 -c 'import cv2, numpy' >/dev/null 2>&1; then
  if [ "${NO_DISPLAY:-0}" = 1 ]; then
    echo '[WARN] cv2/numpy unavailable; allowed because NO_DISPLAY=1'
  else
    echo '[FAIL] Python cv2 and/or numpy is unavailable'
    failed=1
  fi
else
  echo '[OK] Python: cv2 + numpy'
fi

if [ "$failed" -ne 0 ]; then
  echo '[FAIL] Preflight failed. See docs/PORTING_GUIDE_CN.md.'
  exit 1
fi
echo '[OK] Preflight passed for the validated fixed-node topology.'
