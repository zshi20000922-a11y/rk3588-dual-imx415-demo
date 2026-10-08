# RK3588 双 IMX415 人体检测 Demo：框架、不足与优化方向

## 1. 目标与当前实测基线

该 Demo 使用两颗并排放置、未做精确标定的 IMX415：

- CSI3 全局相机：传感器 3840×2160 RAW10、约 60 FPS，经 RKISP 输出 960×540 NV12，用于全局 person 搜索。
- CSI4 ROI 相机：传感器直接读出 648×640 RAW10 ROI、约 110 FPS，经 RKISP 中心裁剪为 640×480 NV12。它不是从 ISP 后的 4K 图像软件裁剪而来。
- RK3588 三个 NPU 核分别持有独立 RKNN context。无人时三核并行处理全局帧；发现人后保留周期性全局跟踪，其余算力用于高速 ROI。
- 已测无人场景全局推理约 60 FPS；ROI 激活时全局约 9～10 FPS、ROI 推理约 96～98 FPS；纯 ROI 压测约 100～106.5 FPS。数据来自原板特定镜像、驱动、时钟和场景，不是对另一块板的无条件保证。

## 2. 软件与数据框架

```text
IMX415/CSI3 -> DPHY/CIF -> RKISP -> /dev/video44 NV12 960x540 --+
                                                                +-> latest-frame -> 3x RKNN YOLOv5n
IMX415/CSI4 -> sensor ROI -> DPHY/CIF -> RKISP -> /dev/video53 --+       |
                                                                        +-> person boxes/FPS
global person center -> normalized affine mapping -> S_SELECTION -> IMX415 PIX_HST/PIX_VST
NV12 + boxes/FPS -> /dev/shm seqlock files -> Python OpenCV/Wayland -> HDMI main + conditional PiP
```

传感器动态 ROI 通过 `VIDIOC_SUBDEV_S_SELECTION` 修改。配套 IMX415 驱动在流运行期间用 group-hold 同步写 `PIX_HST/PIX_VST`，避免停流。应用只保留最新帧，推理落后时丢弃旧帧，因此不会持续积累时延。

## 3. 包内容

- `runtime/`：AArch64 可执行文件、YOLOv5n RKNN、标签、RKNN/RGA 动态库、相机配置/自检/启停脚本和 HDMI 显示程序。
- `source/demo/`：双摄主程序、复用的单摄采集/推理实现和 ROI 控制工具源码。
- `source/rknn-overlay/`：当前模型仓库的 CMake 目标与低拷贝推理修改文件。
- `source/driver/imx415.c`：当前 Linux 6.1 BSP 中已支持 648×640 模式与运行时动态 ROI 的完整驱动源码。它必须基于目标板 BSP 重新审查/合入，不能把 `.c` 文件直接复制到板端生效。

## 4. 已知不足

1. **设备拓扑硬编码**：应用/脚本固定使用 media4/media5、subdev2/subdev7、video44/video53 和实体名。不同设备树、探测顺序或摄像头接口会改变编号。
2. **驱动与固件耦合**：高速 ROI 依赖定制 IMX415 模式、动态 selection 和正确的 CSI/DPHY 时序；仅部署用户态包不能给标准镜像补上这些能力。
3. **没有双目标定**：目前只做归一化坐标、尺度与偏移映射。两相机基线、视差、安装角度和目标距离变化会造成 ROI 偏离。
4. **并非端到端零拷贝**：V4L2 MMAP 帧仍复制到堆内存；HDMI 又经过共享内存和 CPU `cv2.cvtColor/resize`。显示进程可占用一个以上 CPU 核。
5. **调度为吞吐优化**：三核处理不同最新帧，聚合 FPS 高，但单目标轨迹时序和结果排序不严格；全局更新约 100 ms 一次时，快速目标可能跨出副相机 ROI。
6. **线程健壮性仍可提升**：日志写入及部分检测状态需要统一锁或原子快照；异常退出后 AIQ、共享内存和相机状态的恢复不够事务化。
7. **模型精度有限**：YOLOv5n INT8 偏重速度，小目标、遮挡、逆光和非 COCO 分布可能漏检；当前只有 person 类被业务使用。
8. **显示分辨率固定**：HDMI 合成画布固定为 1920×1080，依赖 Wayland 和 Python OpenCV/NumPy。
9. **性能数字缺少正式统计口径**：尚未形成多温度、多曝光、多目标、长时间压力下的 P50/P95 时延、丢帧、功耗和 CSI 错误统计。

## 5. 优化路线（按优先级）

### P0：先解决可迁移性与正确性

- 依据 `media-ctl -p` 的实体名自动发现节点，生成运行时拓扑，不再使用固定编号；同时让 C++ 接受 global/ROI video、sensor-subdev 参数。
- 对新板校验 kernel、DTB、IMX415 驱动、IQ 文件、RKNN runtime/driver ABI 和 RGA 版本；建立一键验收脚本，检查两路 stream FPS 与 `dmesg` 中 CRC/ECC/overflow。
- 做双相机标定。若只跟踪一个平面可用单应矩阵；目标距离变化明显时应使用内外参、深度假设或扩大/预测 ROI。
- 修正多线程共享状态和日志同步，增加看门狗、信号安全清理、AIQ 重启失败回滚。

### P1：降低时延与 CPU 占用

- V4L2 使用 DMABUF，多缓冲引用计数替代每帧 `vector` 分配和拷贝；RGA/RKNN 采用外部内存或零拷贝接口。
- HDMI 改为 DRM/KMS plane、RGA 合成或 GStreamer 硬件路径，移除 Python/OpenCV 全帧色转和缩放。
- 使用固定容量对象池与无锁 latest-frame ring；记录采集、预处理、NPU、后处理、显示的单帧时间戳。

### P2：提升跟踪与检测效果

- 全局检测配合轻量 tracker/Kalman 预测，按目标速度提前移动 ROI；加入迟滞、最大移动速度和丢失后的螺旋/分区搜索。
- 多人时定义主目标选择策略（置信度、面积、中心距离、ID 锁定），而不是只选最高置信度。
- 用实际摄像头数据重新量化/微调更合适的轻量模型，并分别评估召回率和端到端延迟。

### P3：理论性能验证

- 锁定 NPU/DDR/ISP 频率并记录温度与降频；运行 30 分钟以上压力测试。
- 分别测：相机采集上限、单核 NPU、三核独立帧聚合、启用/禁用 HDMI、静态/动态 ROI。
- 报告至少包含 camera FPS、infer FPS、端到端 P50/P95/P99 latency、drop ratio、CPU/NPU/DDR、温度、功耗及 CSI 错误计数。

## 6. 性能边界解释

ROI 相机约 110 FPS 是当前 648×640 RAW10 模式的实际采集上限；166 FPS 是此前另一套 640×640 ROI 寄存器/时序条件下的结果，不能由当前用户态程序单独恢复。要达到 166 FPS，需逐项核对传感器 VMAX/HMAX、lane rate、MIPI blanking、曝光上限、驱动 mode table、接收 PHY 以及 ISP/CIF 吞吐，并确保长时间无 CRC/ECC/overflow。
