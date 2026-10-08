# RK3588 双 IMX415：全局搜索 + 高速动态 ROI 人体检测 Demo

## 1. Demo 当前状态

该 Demo 已在 RK3588 板端 `87f1b382a54776ab` 上完成验证。两颗 IMX415 分别连接 CSI3 和 CSI4：CSI3 执行全局搜索，CSI4 使用传感器端高速 ROI；全局相机检测到 person 后，CSI4 的 ROI 会按目标位置动态移动，并在 HDMI 上以画中画显示。

当前实测基线：

| 项目 | 配置 | 实测结果 |
|---|---|---:|
| CSI3 传感器 | 3840×2160 RAW10 | 约 60 FPS |
| CSI3 ISP 输出 | 960×540 NV12 | 约 60 FPS |
| CSI4 传感器 ROI | 648×640 RAW10，向下游提供有效 640×640 | 约 110.47 FPS |
| CSI4 ISP 输出 | 640×480 NV12 | 约 109～111 FPS |
| 无目标时全局 YOLO | 三个 NPU 核处理不同的最新全局帧 | 约 59～60 FPS |
| ROI 激活时全局 YOLO | 周期性更新全局目标 | 约 9～10 FPS |
| ROI 激活时 ROI YOLO | 其余 NPU 时间处理不同 ROI 帧 | 约 97～101 FPS |

以上数字是当前板端、镜像、驱动、IQ 文件和 YOLOv5n INT8 模型组合下的实测值，不是芯片数据手册上限。

## 2. 本机关键文件

### 2.1 Demo 源码和脚本

目录：`/home/user/PycharmProjects/shize/RK3588/imx415-roi-tool`

- `imx415-dual-person-demo.cc`：双摄采集、三 NPU 调度、person 结果、动态 ROI 和共享内存输出的主程序。
- `imx415-yolo-roi-demo.cc`：被主程序复用的 V4L2、RKNN、日志等基础实现。
- `dual-person-hdmi.py`：读取共享 NV12 和检测框，在 Wayland/HDMI 上合成主画面与 ROI 画中画。
- `dual-imx415-global-roi.sh`：原板双摄配置脚本。
- `run-dual-person-demo.sh`：原始启动脚本。
- `imx415-roi-ctl.c`、`imx415-roi-ctl`：单独设置/验证传感器 ROI selection 的工具。
- `DUAL_PERSON_DEMO_CN.md`：早期运行和性能记录。
- `README_DUAL_CAMERA_DEMO_CN.md`：本文档。

### 2.2 RKNN 模型、推理修改和可执行文件

目录：`/home/user/PycharmProjects/shize/RK3588/rknn_model_zoo-main`

- `examples/yolov5/model/yolov5n.rknn`：RK3588 INT8 YOLOv5n 模型。
- `examples/yolov5/model/coco_80_labels_list.txt`：COCO 标签文件。
- `examples/yolov5/cpp/rknpu2/yolov5.cc`：NV12→RGA letterbox→RKNN 推理路径；包含线程局部输入缓冲复用。
- `examples/yolov5/cpp/CMakeLists.txt`：增加双摄 Demo 构建目标。
- `build/dual-person-rk3588/imx415_dual_person_demo`：当前 AArch64 可执行文件。

### 2.3 内核、设备树和 IMX415 驱动

SDK：`/home/user/PycharmProjects/shize/RK3588/04、linux6.1_sdk/clean-sdk-20260918/atk_dlrk3588_linux6.1`

- `kernel-6.1/drivers/media/i2c/imx415.c`：定制 IMX415 驱动，包含 648×640 RAW10 模式和运行时动态 selection。
- `kernel-6.1/boot.img`：当前刷入板端的 Linux 6.1.141 双 MIPI boot 镜像。
- 使用的配置：`01_atk_dlrk3588_auto2mipi_2hdmi_defconfig`。
- 主设备树：`rk3588-alientek-2mipi720x1280-2hdmi.dtb`；boot 中还包含其他屏幕规格的多 DTB。

### 2.4 ISP IQ 文件

目录：`/home/user/PycharmProjects/shize/RK3588/rknn-cpp-Multithreading-main`

- `imx415-iq-current.json`：当前正确版本；在标准 4K IQ 基础上增加了 640×640 LSC 分辨率和对应表。
- `imx415-iq-backup.json`：SDK 原始备份，只包含 3840×2160 LSC，不可直接用于当前高速 ROI ISP 输出。

板端生效路径：

```text
/etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json
```

若使用原始 IQ，CSI4 的 RAW 数据和帧率仍正常，但 RKISP 会输出标准纯黑 NV12（Y=0、UV=128），同时 AIQ 日志持续出现：

```text
ALSC: can't find 640x640 in lscResName
```

### 2.5 可迁移部署包和备份

- 目录：`/home/user/PycharmProjects/shize/RK3588/dual-person-demo-portable`
- 压缩包：`/home/user/PycharmProjects/shize/RK3588/dual-person-demo-portable-20260920.tar.gz`
- 新板原 boot 备份：`/home/user/PycharmProjects/shize/RK3588/board-backups/87f1b382a54776ab-20260920/boot-before-linux6.1.img`
- 新板原 IQ 备份：`/home/user/PycharmProjects/shize/RK3588/board-backups/87f1b382a54776ab-20260920/iqfiles/imx415-original.json`

迁移包包含程序、模型、标签、RKNN/RGA 备用库、配置脚本、HDMI 程序、ROI IQ、应用源码和 IMX415 驱动参考源码。

## 3. 当前摄像头硬件与 media 配置

### 3.1 CSI3：全局相机

实际链路：

```text
IMX415 I2C 3-001a
  /dev/v4l-subdev2
    -> /dev/media2: rkcif-mipi-lvds2
    -> /dev/media4: rkisp0-vir0
    -> /dev/video44: rkisp_mainpath
```

格式：

```text
Sensor native report : 3864x2192 SGBRG10
CIF/ISP active image : 3840x2160 RAW10
ISP mainpath output  : 960x540 NV12
Frame rate           : about 60 FPS
```

3864×2192 包含传感器边界；实际可见区域使用 3840×2160。960×540 是 ISP 缩放输出，YOLO 检测框坐标最终映射回这一路的 960×540 源坐标。

### 3.2 CSI4：高速 ROI 相机

实际链路：

```text
IMX415 I2C 4-001a
  /dev/v4l-subdev7
    -> /dev/media3: rkcif-mipi-lvds4
    -> /dev/media5: rkisp0-vir1
    -> /dev/video53: rkisp_mainpath
```

格式：

```text
Sensor transport     : 648x640 SGBRG10
CIF effective image  : 640x640 RAW10 (discard horizontal border pixels)
ISP mainpath output  : 640x480 NV12
Frame rate           : about 110.47 FPS
MIPI lane rate       : driver log reports about 1782 Mbps/lane
```

这里的 ROI 是 IMX415 传感器窗口读出，不是先获取 4K，再从 ISP 输出中软件裁剪。传感器要求水平输出满足硬件对齐，所以 MIPI 传输宽度是 648，CIF/ISP 使用其中有效的 640 像素。

驱动默认中心 ROI 的可见位置约为 `(1612,776)/640×640`。运行时通过：

```text
VIDIOC_SUBDEV_S_SELECTION
```

在不中断视频流的情况下移动 ROI。驱动使用 IMX415 group-hold 同步写入 `PIX_HST/PIX_VST`，避免移动过程中出现半帧寄存器状态。

### 3.3 设备树中的未连接候选节点

启动日志可能同时出现：

```text
imx415 2-001a: Unexpected sensor id(000000)
imx415 7-001a: Unexpected sensor id(000000)
```

当前实际使用并成功探测的是 `3-001a` 和 `4-001a`。`2-001a`、`7-001a` 是设备树为其他 MIPI 连接方式保留的候选节点；在 CSI3/CSI4 当前连接正常时，这两条失败日志不代表实际两颗相机故障。

## 4. 应用架构

```text
CSI3 IMX415 4K60
    -> RKCIF -> RKISP -> NV12 960x540 -> LatestCamera ----+
                                                        |
CSI4 IMX415 648x640 ROI @110                           |
    -> RKCIF -> RKISP -> NV12 640x480 -> LatestCamera --+--> 3 RKNN contexts
                                                               NPU core 0/1/2
                                                                    |
                                                 person boxes + confidence + FPS
                                                                    |
                     global person center -> normalized mapping -> dynamic sensor ROI
                                                                    |
                         NV12 + boxes -> /dev/shm seqlock -> Python HDMI compositor
```

### 4.1 NPU 调度

- 程序创建三个独立 RKNN context，分别绑定 NPU0、NPU1、NPU2。
- ROI 未激活时，三个工作线程领取不同的最新全局帧，聚合吞吐接近全局相机 60 FPS。
- 全局检测到 person 后，NPU0 按 `GLOBAL_PERIOD_MS` 周期继续更新全局目标。
- 其余可用推理时间处理不同的最新 ROI 帧，避免三个核心重复推理同一帧。
- 只处理最新帧，不排队旧帧，因此推理不足时优先丢帧而不是积累延迟。

### 4.2 动态 ROI 映射

全局 person 框中心先归一化，再映射到副摄完整 3840×2160 坐标系：

```text
global box center
  -> normalized x/y
  -> ROI_SCALE_X / ROI_SCALE_Y
  -> ROI_OFFSET_X / ROI_OFFSET_Y
  -> clamp to sensor bounds
  -> even alignment
  -> VIDIOC_SUBDEV_S_SELECTION
```

当前两相机没有双目标定，默认参数为：

```text
ROI_SCALE_X=1.0
ROI_SCALE_Y=1.0
ROI_OFFSET_X=0
ROI_OFFSET_Y=0
ROI_MOVE_THRESHOLD=48
```

`ROI_MOVE_THRESHOLD` 是移动死区，防止检测框轻微抖动导致传感器 ROI 每帧更新。

### 4.3 图像预处理和显示

- YOLO 输入直接使用 NV12，由 RGA 完成颜色转换和 640×640 letterbox。
- 每个推理线程复用输入缓冲，避免每帧 `malloc/free`。
- V4L2 采集线程将最新帧保存为共享只读对象，多个推理线程只复制智能指针。
- C++ 将两路最新 NV12、检测框、相机 FPS、推理 FPS 写入 `/dev/shm/dual-person-global` 和 `/dev/shm/dual-person-roi`。
- Python/OpenCV 显示程序读取共享内存；默认只显示全局画面，检测到 person 后显示 ROI 画中画。

## 5. 板端文件和启动方式

板端目录：

```text
/userdata/dual-person-demo/
├── imx415_dual_person_demo
├── dual-person-hdmi.py
├── configure-cameras.sh
├── preflight.sh
├── run.sh
├── stop.sh
├── model/
│   ├── yolov5n.rknn
│   └── coco_80_labels_list.txt
└── lib/
    ├── librknnrt.so
    └── librga.so
```

连接：

```bash
adb devices
adb -s 87f1b382a54776ab shell
```

启动：

```bash
cd /userdata/dual-person-demo
./preflight.sh
nohup ./run.sh >/tmp/dual-person-launcher.log 2>&1 &
echo $! >/tmp/dual-person-demo.pid
```

停止：

```bash
cd /userdata/dual-person-demo
./stop.sh
```

主要日志：

```text
/tmp/dual-person-demo.jsonl       检测、ROI 坐标和 FPS
/tmp/dual-person-launcher.log     主程序/RKNN/RGA 输出
/tmp/dual-person-hdmi.log         HDMI 显示输出
/tmp/rkaiq_dual_global_roi.log    RKAIQ/ISP 输出
```

## 6. 当前不足和可优化点

### P0：稳定性和可迁移性

1. **设备节点仍然硬编码**：当前固定使用 media4/media5、subdev2/subdev7、video44/video53。应通过 media entity 名和传感器 I2C 名自动发现节点，并通过命令行传给 C++。
2. **驱动、DTB、IQ 必须成套部署**：只复制应用会导致缺少高速模式；只刷 boot、不复制 ROI IQ 会出现 RAW 正常而 ISP 纯黑。应制作带版本清单和 SHA-256 的统一部署/回滚工具。
3. **启动脚本重启整个 AIQ 服务**：会影响板端其他摄像头业务。应改为明确的服务管理、互斥锁和失败回滚。
4. **共享状态需要严格同步**：检测状态和日志应统一使用锁或原子快照，消除潜在数据竞争和多线程日志交叉。
5. **增加运行时健康检测**：自动检查 ROI Y 平面是否长期为 0、相机 FPS、AIQ 错误、CSI CRC/ECC，并在异常时恢复相机管线。

### P1：跨相机定位准确度

1. 当前仅使用归一化坐标、尺度和偏移，两相机存在基线、视差和安装角度误差。
2. 固定平面场景可通过单应矩阵校准；目标距离变化明显时需要双目内外参、目标深度估计或按距离扩大 ROI。
3. 加入 tracker/Kalman 预测，根据目标速度提前移动 ROI，降低全局检测约 100 ms 更新周期带来的滞后。
4. 多人场景需要明确主目标策略，例如 ID 锁定、画面中心、置信度、目标面积或业务优先级。

### P2：内存和显示性能

1. 目前 V4L2 MMAP 帧仍复制到堆内存；可改为 DMABUF、多缓冲引用计数和 RKNN/RGA 外部内存。
2. `LatestCamera` 每帧创建新的 `vector/shared_ptr`；可改为固定缓冲池或无锁 ring buffer。
3. HDMI 使用 Python/OpenCV 做 NV12→BGR、缩放和 1920×1080 合成，CPU 占用较高；可改为 RGA + DRM/KMS plane 或硬件 GStreamer。
4. 当前 RGA letterbox 的背景填充在日志中可能出现 `RGA_COLORFILL Invalid argument`，推理仍正常，但应更新 imageutils/RGA 调用方式或用预分配背景帧消除警告和潜在 CPU fallback。

### P3：检测性能和画质

1. YOLOv5n INT8 速度高但小目标、遮挡和逆光召回率有限；应使用实际相机数据做量化校准和微调。
2. 640×640 ROI 的 IQ 目前主要为兼容性配置，LSC 使用平坦表；若关注画质，应针对不同 ROI 位置建立更合理的 shading/黑电平/噪声参数。
3. 高速模式最大曝光受 110 FPS 帧周期限制，暗光下会依赖高增益并增加噪声。可以在检测置信度、帧率和曝光之间动态切换 60/90/110 FPS 档位。
4. 当前副摄一直预启动 ROI 流以避免模式切换延迟，会持续占用 ISP/DDR/功耗；可评估低功耗待机、预热时间和首次检测延迟之间的折中。

### P4：正式性能报告

建议增加统一基准工具并至少记录：

- camera FPS、infer FPS、有效/丢弃帧数；
- 采集→显示端到端 P50/P95/P99 延迟；
- NPU0/1/2 利用率、CPU、DDR、ISP、温度和功耗；
- 静态 ROI、动态 ROI、多人和目标快速移动测试；
- 30 分钟以上压力测试中的 CRC/ECC/overflow/size error；
- 不同曝光、室内外、暗光和逆光条件下 person recall/precision。

## 7. 重要结论

- CSI4 的 640×640 是传感器端 ROI，不是 ISP 后从 4K 软件裁剪。
- 当前稳定的输入上限约为 110.47 FPS；曾测得的 166 FPS 属于另一组传感器时序/寄存器条件，不能仅靠应用参数恢复。
- 当前系统必须同时具备 Linux 6.1 定制 boot、双 MIPI DTB、定制 IMX415 驱动和含 640×640 条目的 IQ 文件。
- 判断 ROI 黑屏时应分层检查：共享 NV12 → `/dev/video53` → CIF RAW `/dev/video33`。本次问题中 RAW 正常而 ISP 纯黑，最终定位到 IQ 缺少 ROI 分辨率条目。
