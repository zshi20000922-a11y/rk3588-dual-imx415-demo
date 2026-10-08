# 分层调试手册

## 1. 先确认板和进程

```bash
adb devices -l
adb -s SERIAL shell 'uname -a'
adb -s SERIAL shell 'ps -ef | grep -E "imx415_dual_person_demo|dual-person-hdmi|rkaiq" | grep -v grep'
```

不要在旧 Demo 仍占用 video 节点时重复启动。

## 2. 确认两颗传感器

```bash
dmesg | grep -E 'imx415 [34]-001a|Detected imx415|Unexpected sensor'
```

当前正确结果是 `3-001a` 和 `4-001a` 检测到 ID `0xE0`。如果某颗读取 `000000`：

- 断电检查排线方向和锁扣；
- 互换摄像头/排线区分模组、排线和接口；
- 检查供电、复位、时钟和 I²C；
- 重启后重新看 probe 日志。

## 3. 确认 media graph

```bash
media-ctl -p -d /dev/media2
media-ctl -p -d /dev/media3
media-ctl -p -d /dev/media4
media-ctl -p -d /dev/media5
```

预期：

```text
media2: imx415 3-001a -> rkcif-mipi-lvds2
media3: imx415 4-001a -> rkcif-mipi-lvds4
media4: rkcif-mipi-lvds2 -> rkisp -> video44
media5: rkcif-mipi-lvds4 -> rkisp -> video53
```

## 4. 确认模式和输出格式

```bash
v4l2-ctl -d /dev/v4l-subdev2 --get-subdev-fmt pad=0
v4l2-ctl -d /dev/v4l-subdev7 --get-subdev-fmt pad=0
v4l2-ctl -d /dev/video44 --get-fmt-video
v4l2-ctl -d /dev/video53 --get-fmt-video
```

预期：

```text
subdev2 3864x2192 SGBRG10
subdev7 648x640 SGBRG10
video44 960x540 NV12
video53 640x480 NV12
```

## 5. 双路并发 FPS

在两个终端同时运行：

```bash
v4l2-ctl -d /dev/video44 --stream-mmap=4 --stream-count=180 --stream-poll --verbose
v4l2-ctl -d /dev/video53 --stream-mmap=4 --stream-count=330 --stream-poll --verbose
```

应分别接近 60 FPS 和 110.47 FPS。必须并发测，串行成功不能证明双路带宽稳定。

## 6. ROI 黑屏的三层定位

### 第 1 层：共享内存/HDMI

如果 `/dev/video53` 图像正常但 PiP 黑，检查：

- `/dev/shm/dual-person-roi` 是否更新；
- header 宽高和 frame size；
- Python NV12 reshape 与 `cv2.COLOR_YUV2BGR_NV12`；
- PiP active 标记和显示日志。

### 第 2 层：ISP NV12

```bash
v4l2-ctl -d /dev/video53 --stream-mmap=4 --stream-skip=30 \
  --stream-count=1 --stream-to=/tmp/roi.nv12
```

纯黑 NV12 的典型统计是 Y 全 0、UV 全 128。

### 第 3 层：CIF RAW

```bash
v4l2-ctl -d /dev/video33 --stream-mmap=4 --stream-skip=10 \
  --stream-count=1 --stream-to=/tmp/roi.raw
```

如果 RAW 大量非零而 NV12 纯黑，检查 RKAIQ/IQ/ISP，而不是继续改 MIPI 寄存器。本项目已遇到的根因是 IQ 缺少 `640x640` LSC 条目。

```bash
grep -E 'ALSC|640x640|fail|error' /tmp/rkaiq_dual_global_roi.log
```

## 7. 动态 ROI 不移动或位置错误

查看应用日志：

```bash
tail -f /tmp/dual-person-demo.jsonl
```

查看传感器 selection：

```bash
v4l2-ctl -d /dev/v4l-subdev7 --get-subdev-selection=target=crop,pad=0
```

若 selection 在变化但目标不在 ROI：

- 调整 `ROI_SCALE_X/Y`、`ROI_OFFSET_X/Y`；
- 检查两摄安装方向是否镜像/旋转；
- 减小或增大 `ROI_MOVE_THRESHOLD`；
- 做单应矩阵或双目标定；
- 加入 tracker 预测。

## 8. YOLO 帧率低

```bash
tail -f /tmp/dual-person-demo.jsonl
cat /sys/kernel/debug/rknpu/version 2>/dev/null
```

确认：

- 创建了 3 个独立 RKNN context；
- 三个 context 分别绑定 NPU core 0/1/2；
- 多个 worker 没有重复领取同一帧；
- NV12 直接走 RGA，而不是 CPU 逐像素转换；
- HDMI Python 是否吃满 CPU；
- NPU/DDR/CPU 是否因温度降频。

## 9. RGA COLORFILL 警告

当前可能出现：

```text
RGA_COLORFILL fail: Invalid argument
```

当前推理仍可运行并达到目标吞吐，但可能存在 imageutils fallback。优化方向：

- 对齐 imageutils、librga 和内核 RGA driver 版本；
- 预先填充复用的 letterbox 背景缓冲，避免每帧 color fill；
- 验证 colorspace、stride、handle 和 buffer import 参数；
- 用阶段耗时确认是否回退 CPU。

## 10. CSI 错误

```bash
dmesg | grep -Ei 'crc|ecc|overflow|size error|mipi|csi'
```

若出现错误，依次检查 lane rate、lane 数、sensor/CIF 格式尺寸、HMAX/VMAX、时钟、排线和 PHY 接口。不要只根据平均 FPS 判断链路稳定。

## 11. 安全恢复

- 修改 IQ 前保留 `/etc/iqfiles` 原文件；
- 刷 boot 前备份 `/dev/block/by-name/boot`；
- 不要把具体测试板的 recovery 文件刷到不确认兼容的其他板；
- boot 失败时使用 Loader/Maskrom 和已备份 boot 恢复。
