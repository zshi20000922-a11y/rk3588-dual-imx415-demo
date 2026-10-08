# 文件索引与作用

## 顶层

| 文件 | 作用 |
|---|---|
| `README_FIRST_CN.md` | 交接入口、阅读顺序、最短部署路径和重要限制。 |
| `deploy-adb.sh` | 主机端部署脚本；复制 runtime、备份原 IQ、安装 ROI IQ并设置执行权限。 |

## runtime：板端运行文件

| 文件 | 作用 |
|---|---|
| `runtime/imx415_dual_person_demo` | AArch64 主程序；双路 V4L2、3 个 RKNN context、动态 ROI 和共享内存输出。 |
| `runtime/dual-person-hdmi.py` | Wayland/OpenCV HDMI 合成器；显示全局框、FPS，以及目标出现后的 ROI PiP。 |
| `runtime/run.sh` | 总启动器；自检、配置相机、启动检测器和显示器、处理退出清理。 |
| `runtime/stop.sh` | 停止检测器和 HDMI 显示进程。 |
| `runtime/preflight.sh` | 检查架构、命令、NPU、固定媒体节点、模型、动态库和 Python 依赖。 |
| `runtime/configure-cameras.sh` | 将 CSI3 配成 4K60 全局路，将 CSI4 配成 648×640 高速 ROI 路，并重启 RKAIQ。 |
| `runtime/model/yolov5n.rknn` | RK3588 INT8 YOLOv5n 模型，输入 640×640。 |
| `runtime/model/coco_80_labels_list.txt` | COCO 80 类标签；业务只使用 person。 |
| `runtime/lib/librknnrt.so` | RKNN runtime 备用库；正常优先使用系统镜像匹配版本。 |
| `runtime/lib/librga.so` | RGA 备用库；正常优先使用系统镜像匹配版本。 |
| `runtime/iqfiles/*.json` | 包含 640×640 ROI LSC 条目的 IMX415 IQ 文件。 |

## source/application：业务源码

| 文件 | 作用 |
|---|---|
| `imx415-dual-person-demo.cc` | 双摄主逻辑。包含 latest-frame 采集、三核调度、动态 ROI 映射、状态 JSON 和共享内存。 |
| `imx415-yolo-roi-demo.cc` | 被双摄主程序 include 的基础模块。包含 V4L2 Capture、RKNN 包装、参数处理和单摄 Demo。 |
| `imx415-roi-ctl.c` | 直接调用 `VIDIOC_SUBDEV_S_SELECTION` 的最小控制/验证工具源码。 |

## source/rknn_overlay：model zoo 修改层

| 文件 | 作用 |
|---|---|
| `CMakeLists.txt` | 增加 `imx415_dual_person_demo` 构建目标；交接版已移除本机绝对路径。 |
| `rknpu2/yolov5.cc` | RKNN YOLOv5 推理实现；使用线程局部复用输入缓冲，支持 NV12 经 imageutils/RGA letterbox。 |
| `reference_headers/yolov5.h` | 当时构建所用接口头文件快照，便于审阅。 |
| `reference_headers/postprocess.cc/.h` | YOLOv5 后处理和标签加载快照，便于复现/比较。 |

`reference_headers` 不是完整 model zoo。构建时仍应以完整上游项目为基础。

## source/kernel

| 文件 | 作用 |
|---|---|
| `imx415.c` | Linux 6.1 定制驱动。新增 2568×1440、648×640 RAW10 模式，以及运行中 group-hold 更新 `PIX_HST/PIX_VST`。 |

## iq

| 文件 | 作用 |
|---|---|
| `imx415-roi-640x640.json` | 当前正确 IQ；包含 3840×2160 和 640×640 LSC 分辨率。 |
| `imx415-sdk-original.json` | SDK 原始 IQ 对照；缺少 640×640，ROI 模式会出现 ISP 纯黑。 |

## firmware

| 文件 | 作用 |
|---|---|
| `boot-linux6.1.141-dual-mipi-roi.img` | 已验证 boot 分区镜像，包含 Linux 6.1.141、双 MIPI DTB 和定制 IMX415 驱动。 |

这是 boot 分区，不是完整 rootfs/update 镜像。刷写前必须确认板型、分区名并备份原 boot。

## docs

| 文件 | 作用 |
|---|---|
| `README_DUAL_CAMERA_DEMO_CN.md` | 最完整的架构、配置、性能、问题和优化说明。 |
| `BUILD_AND_RELEASE_CN.md` | 源码覆盖、交叉编译、部署包制作和版本核查。 |
| `DEBUG_PLAYBOOK_CN.md` | 分层故障诊断命令和判断标准。 |
| `VERSION_AND_CHECKSUMS_CN.md` | 本次交付基线、关键文件摘要和刷写边界。 |
| `SHA256SUMS` | 可直接由 `sha256sum -c` 使用的完整性清单。 |
| `PORTING_GUIDE_CN.md` | 换板部署、驱动迁移和验收。 |
| `ARCHITECTURE_AND_LIMITS_CN.md` | 早期架构与限制分析。 |
| `ORIGINAL_TEST_NOTES_CN.md` | 原始测试记录，保留用于追溯。 |

## release 和 recovery

| 文件 | 作用 |
|---|---|
| `release/dual-person-demo-portable-20260920.tar.gz` | 只包含应用 runtime、源码修改层和文档的轻量部署包。 |
| `recovery/board-87f1b382-original-iq.json` | 测试板替换前的 IQ 备份，仅用于该板恢复参考。 |
