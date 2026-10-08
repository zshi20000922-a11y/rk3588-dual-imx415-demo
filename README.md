# RK3588 双 IMX415 全局搜索与动态 ROI 检测 Demo

本项目是在 RK3588 上运行的双 MIPI 摄像头人体检测 Demo：CSI3 摄像头持续进行全局搜索；检测到目标后，将目标位置映射到 CSI4 摄像头的 Sensor ROI，并进行高速局部检测。画面通过 HDMI 输出，包含检测框、帧率和 ROI 画中画。

## 当前验证基线

- SoC：Rockchip RK3588
- 系统：Linux 6.1.141
- 摄像头：双 IMX415，分别连接 CSI3 和 CSI4
- 全局路：3840×2160 RAW10 输入，实测约 60 FPS；ISP 输出 NV12 960×540
- ROI 路：648×640 RAW10 Sensor 窗口，有效区域 640×640，实测约 109–111 FPS；ISP 输出 NV12 640×480
- 推理：YOLOv5n RKNN，使用 RK3588 NPU；ROI 激活时测得约 97–101 FPS

以上为特定板卡、驱动、IQ 和运行配置下的实测结果，不代表所有 RK3588 板卡均能达到相同性能。ROI 路的 648×640 是 Sensor 输出窗口设置，有效图像为 640×640；ISP 的 640×480 是另一阶段的输出尺寸。

## 数据流程

```text
CSI3 IMX415 → MIPI/CIF → ISP → NV12 全局图 → YOLOv5n 全局检测
                                             │ 目标位置
                                             ▼
CSI4 IMX415 ← 动态 Sensor ROI ← 坐标映射
    → MIPI/CIF → ISP → NV12 ROI 图 → YOLOv5n 高速检测
                                  → HDMI 合成与 ROI 画中画
```

Sensor ROI 由 V4L2 Sub-device Selection 更新。ROI 模式的 ISP 正常出图还依赖包含 640×640 LSC 条目的 IMX415 IQ 文件。

## 目录导航

| 路径 | 内容 |
|---|---|
| `runtime/` | 可部署至板端的程序、YOLO 模型、运行库、启动和相机配置脚本 |
| `source/application/` | 双摄 Demo、单摄基础模块和 ROI 控制工具源码 |
| `source/rknn_overlay/` | 面向 `rknn_model_zoo` 的构建和推理修改层 |
| `source/kernel/imx415.c` | 定制 IMX415 V4L2 驱动源码 |
| `iq/` | ROI 可用 IQ 与 SDK 原始 IQ 对照 |
| `firmware/` | 已验证的 Linux 6.1 boot 分区镜像 |
| `release/` | 轻量应用部署包 |
| `recovery/` | 测试板原 IQ 恢复参考文件 |
| `docs/` | 架构、文件索引、构建、部署、调试和移植说明 |

## 快速部署

在主机安装并连接 ADB，确认板端已经刷入适配的 boot 镜像，摄像头连接 CSI3/CSI4：

```bash
adb devices
./deploy-adb.sh <板端序列号>
adb -s <板端序列号> shell 'cd /userdata/dual-person-demo && ./preflight.sh'
adb -s <板端序列号> shell 'cd /userdata/dual-person-demo && ./run.sh'
```

`deploy-adb.sh` 会部署 runtime，并将 ROI IQ 安装到板端。部署前会保留板端 IQ 备份。更换板卡后，先按照移植指南核对 media、video、subdev 节点映射，不要直接假设节点编号相同。

## 文档阅读顺序

1. `README_FIRST_CN.md`：交付入口与最短部署说明。
2. `docs/README_DUAL_CAMERA_DEMO_CN.md`：完整架构、摄像头配置、性能和优化分析。
3. `docs/FILE_INDEX_CN.md`：逐文件作用索引。
4. `docs/BUILD_AND_RELEASE_CN.md`：构建和发布流程。
5. `docs/DEBUG_PLAYBOOK_CN.md`：黑屏、低帧率、无帧和 ROI 偏移排查。
6. `docs/PORTING_GUIDE_CN.md`：换板和驱动移植。
7. `docs/VERSION_AND_CHECKSUMS_CN.md`：版本基线与文件完整性说明。

## 完整性校验

在仓库根目录执行：

```bash
sha256sum -c docs/SHA256SUMS
```

## 注意事项

- `firmware/boot-linux6.1.141-dual-mipi-roi.img` 是 boot 分区镜像，不是完整系统升级包。刷写前核对板型、分区布局和设备树。
- 可运行 runtime 与可重编译源码修改层都已提供；完整 Rockchip Linux SDK 和完整 `rknn_model_zoo` 未包含在仓库中，重编译依赖对应基础工程。
- 当前双摄坐标映射按并排相机的 Demo 场景实现，未做精确双目标定；ROI 定位精度会受相机安装姿态和视差影响。
- 当前性能为已有板卡测试记录中的测量结果。系统负载、散热、驱动、IQ、RKNN runtime 版本变化均可能影响帧率。
