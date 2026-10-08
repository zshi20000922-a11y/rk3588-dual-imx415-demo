#!/usr/bin/env python3
import mmap
import os
import struct
import time

import cv2
import numpy as np

MAGIC = 0x44554C31
HEADER = 1024


class SharedCamera:
    def __init__(self, path, width, height):
        self.width = width
        self.height = height
        self.size = HEADER + width * height * 3 // 2
        while not os.path.exists(path):
            time.sleep(0.1)
        self.file = open(path, "rb", buffering=0)
        self.memory = mmap.mmap(self.file.fileno(), self.size, access=mmap.ACCESS_READ)

    def read(self):
        for _ in range(4):
            seq1 = struct.unpack_from("<I", self.memory, 4)[0]
            if seq1 & 1:
                continue
            magic, _, width, height, frame_size, count, active = struct.unpack_from(
                "<7I", self.memory, 0
            )
            if magic != MAGIC or not frame_size:
                return None
            camera_fps, infer_fps = struct.unpack_from("<2f", self.memory, 24)
            boxes = []
            for i in range(min(count, 16)):
                boxes.append(struct.unpack_from("<5f", self.memory, 32 + i * 20))
            raw = self.memory[HEADER:HEADER + frame_size]
            seq2 = struct.unpack_from("<I", self.memory, 4)[0]
            if seq1 == seq2 and not (seq2 & 1):
                nv12 = np.frombuffer(raw, np.uint8).reshape(height * 3 // 2, width)
                bgr = cv2.cvtColor(nv12, cv2.COLOR_YUV2BGR_NV12)
                return bgr, boxes, camera_fps, infer_fps, bool(active)
        return None


def draw_panel(result, panel_width, panel_height, title, color):
    panel = np.zeros((panel_height, panel_width, 3), np.uint8)
    if result is None:
        cv2.putText(panel, "Waiting for camera...", (40, 80),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2)
        return panel
    image, boxes, camera_fps, infer_fps, active = result
    height, width = image.shape[:2]
    scale = min(panel_width / width, (panel_height - 80) / height)
    shown_w, shown_h = int(width * scale), int(height * scale)
    x0, y0 = (panel_width - shown_w) // 2, 70 + (panel_height - 70 - shown_h) // 2

    # RKNN postprocess has already mapped boxes back to source coordinates.
    for left, top, right, bottom, confidence in boxes:
        sx1 = max(0, min(width - 1, left))
        sy1 = max(0, min(height - 1, top))
        sx2 = max(0, min(width - 1, right))
        sy2 = max(0, min(height - 1, bottom))
        if sx2 <= sx1 or sy2 <= sy1:
            continue
        p1 = (int(sx1), int(sy1))
        p2 = (int(sx2), int(sy2))
        cv2.rectangle(image, p1, p2, color, 3)
        label = f"person {confidence:.2f}"
        cv2.putText(image, label, (p1[0], max(28, p1[1] - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2, cv2.LINE_AA)

    shown = cv2.resize(image, (shown_w, shown_h), interpolation=cv2.INTER_LINEAR)
    panel[y0:y0 + shown_h, x0:x0 + shown_w] = shown
    cv2.putText(panel, title, (24, 42), cv2.FONT_HERSHEY_SIMPLEX,
                1.0, color, 2, cv2.LINE_AA)
    status = f"CAM {camera_fps:5.1f} FPS   YOLO {infer_fps:4.1f} FPS"
    cv2.putText(panel, status, (panel_width - 455, 42),
                cv2.FONT_HERSHEY_SIMPLEX, 0.65, (255, 255, 255), 2, cv2.LINE_AA)
    return panel


def main():
    os.environ.setdefault("XDG_RUNTIME_DIR", "/var/run")
    os.environ.setdefault("WAYLAND_DISPLAY", "wayland-0")
    os.environ.setdefault("QT_QPA_PLATFORM", "wayland")
    global_camera = SharedCamera("/dev/shm/dual-person-global", 960, 540)
    roi_camera = SharedCamera("/dev/shm/dual-person-roi", 640, 480)
    window = "RK3588 Dual Person Detection"
    cv2.namedWindow(window, cv2.WINDOW_NORMAL)
    cv2.setWindowProperty(window, cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
    while True:
        global_result = global_camera.read()
        roi_result = roi_camera.read()
        canvas = draw_panel(global_result, 1920, 1080, "GLOBAL SEARCH", (0, 220, 255))
        roi_active = roi_result is not None and roi_result[4]
        if roi_active:
            pip = draw_panel(roi_result, 640, 480, "ROI ACTIVE", (0, 255, 0))
            x0, y0 = 1920 - 660, 1080 - 500
            cv2.rectangle(canvas, (x0 - 4, y0 - 4), (x0 + 644, y0 + 484),
                          (0, 255, 0), 4)
            canvas[y0:y0 + 480, x0:x0 + 640] = pip
        else:
            cv2.putText(canvas, "ROI IDLE - waiting for person", (1330, 1030),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (180, 180, 180), 2,
                        cv2.LINE_AA)
        cv2.imshow(window, canvas)
        if cv2.waitKey(1) & 0xFF in (27, ord("q")):
            break
        time.sleep(0.02)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
