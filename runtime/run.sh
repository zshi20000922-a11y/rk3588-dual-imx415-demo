#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DURATION=${DURATION:-0}
GLOBAL_PERIOD_MS=${GLOBAL_PERIOD_MS:-100}
THRESHOLD=${THRESHOLD:-0.30}
ROI_SCALE_X=${ROI_SCALE_X:-1.0}
ROI_SCALE_Y=${ROI_SCALE_Y:-1.0}
ROI_OFFSET_X=${ROI_OFFSET_X:-0}
ROI_OFFSET_Y=${ROI_OFFSET_Y:-0}
ROI_MOVE_THRESHOLD=${ROI_MOVE_THRESHOLD:-48}

"$APP_DIR/preflight.sh"
"$APP_DIR/configure-cameras.sh"

cd "$APP_DIR"
# Prefer the libraries shipped with the board image because they match its
# kernel drivers. Keep the bundled copies as a fallback for minimal images.
export LD_LIBRARY_PATH="/usr/lib:/lib:$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/var/run}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-wayland}

./imx415_dual_person_demo \
  --model model/yolov5n.rknn \
  --global-period-ms "$GLOBAL_PERIOD_MS" \
  --threshold "$THRESHOLD" \
  --roi-scale-x "$ROI_SCALE_X" \
  --roi-scale-y "$ROI_SCALE_Y" \
  --roi-offset-x "$ROI_OFFSET_X" \
  --roi-offset-y "$ROI_OFFSET_Y" \
  --roi-move-threshold "$ROI_MOVE_THRESHOLD" \
  --duration "$DURATION" &
DETECT_PID=$!
DISPLAY_PID=

cleanup() {
  [ -z "$DISPLAY_PID" ] || kill "$DISPLAY_PID" 2>/dev/null || true
  kill "$DETECT_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

i=0
while [ ! -e /dev/shm/dual-person-global ] && [ "$i" -lt 50 ]; do
  sleep 0.1
  i=$((i + 1))
done

if [ "${NO_DISPLAY:-0}" != 1 ]; then
  python3 "$APP_DIR/dual-person-hdmi.py" >/tmp/dual-person-hdmi.log 2>&1 &
  DISPLAY_PID=$!
fi
wait "$DETECT_PID"
