# RK3588 双 IMX415 Demo 学习与调试交接包

## 先看结论

这是当前已验证可运行版本的交接目录，目标是让其他同学可以：

1. 理解双摄、ISP、动态 ROI、三 NPU 推理和 HDMI 显示的数据通路；
2. 将现成 runtime 部署到同型号 RK3588 板；
3. 在完整 Rockchip SDK 和 `rknn_model_zoo` 基础上重新编译；
4. 按 Sensor → MIPI/CIF → ISP → NV12 → RKNN → HDMI 的顺序分层调试。

当前验证板：RK3588，Linux 6.1.141，双 IMX415 分别接 CSI3/CSI4。

## 推荐阅读顺序

1. `README_FIRST_CN.md`：目录入口和最短使用路径。
2. `docs/README_DUAL_CAMERA_DEMO_CN.md`：完整架构、摄像头配置、性能和优化建议。
3. `docs/FILE_INDEX_CN.md`：逐个文件说明。
4. `docs/BUILD_AND_RELEASE_CN.md`：重新编译和制作部署包。
5. `docs/DEBUG_PLAYBOOK_CN.md`：出现黑屏、无帧、低 FPS、ROI 偏移时如何定位。
6. `docs/PORTING_GUIDE_CN.md`：换板、刷 boot 和验收。
7. `docs/VERSION_AND_CHECKSUMS_CN.md`：交付基线和关键文件完整性校验。
8. `docs/ARCHITECTURE_AND_LIMITS_CN.md`：当前不足、优化路线和验收指标。

## 目录结构

```text
rk3588-dual-imx415-demo-handoff/
├── README_FIRST_CN.md
├── deploy-adb.sh
├── docs/                       文档和调试手册
├── runtime/                    可直接部署到板端的运行目录
├── source/
│   ├── application/            双摄应用和 ROI 工具源码
│   ├── rknn_overlay/           覆盖到 rknn_model_zoo 的修改层
│   └── kernel/                 定制 IMX415 Linux 驱动
├── iq/                         ROI IQ 和 SDK 原始 IQ 对照
├── firmware/                   已验证 Linux 6.1 boot 分区镜像
├── release/                    已打包的纯应用部署包
└── recovery/                   特定测试板的恢复参考文件
```

## 最短部署路径

适用于已经刷入本包 `firmware/` 中 boot、摄像头连接 CSI3/CSI4 的同型号板：

```bash
adb devices
./deploy-adb.sh 板端序列号
adb -s 板端序列号 shell 'cd /userdata/dual-person-demo && ./preflight.sh'
adb -s 板端序列号 shell 'cd /userdata/dual-person-demo && nohup ./run.sh >/tmp/dual-person-launcher.log 2>&1 &'
```

`deploy-adb.sh` 会部署应用，并在备份板端原 IQ 后安装包含 640×640 ROI 条目的 IQ 文件。

## 当前摄像头固定映射

```text
CSI3 / IMX415 3-001a / subdev2 / media2 CIF / media4 ISP / video44
CSI4 / IMX415 4-001a / subdev7 / media3 CIF / media5 ISP / video53
```

```text
CSI3: 3840x2160 RAW10 @60 -> ISP NV12 960x540
CSI4: 648x640 RAW10 sensor ROI @110.47 -> effective 640x640 -> ISP NV12 640x480
```

节点编号由当前设备树和探测顺序决定。换板后必须先用 `media-ctl -p` 核对，不能默认所有 RK3588 都相同。

## 三个必须一起使用的部分

1. 定制 Linux 6.1 boot：包含双 MIPI DTB 和支持 648×640/动态 selection 的 IMX415 驱动；
2. ROI IQ：包含 `640x640` LSC 条目；
3. 用户态 runtime：双摄应用、YOLOv5n RKNN、标签、脚本和 HDMI 显示。

缺少驱动时不会出现高速 ROI 模式；缺少 ROI IQ 时 CIF RAW 正常，但 ISP NV12 会纯黑；缺少 runtime 则没有推理和显示。

## 已验证性能

```text
CSI3 camera             ~60 FPS
CSI4 ROI camera         ~109-111 FPS
Global YOLO when idle   ~59-60 FPS
Global YOLO when active ~9-10 FPS
ROI YOLO when active    ~97-101 FPS
```

## 交接边界

- 本包不包含完整约数十 GB 的 Rockchip Linux SDK 和完整 `rknn_model_zoo`；只保存本项目修改层和可运行产物。
- 重编译需要使用原始 SDK/model zoo，并按构建文档覆盖文件。
- `recovery/` 中的文件带有具体测试板背景，只用于恢复参考，不应无条件刷入其他板。
- 完整 rootfs update 镜像没有在本包中生成；`firmware/` 提供的是已验证 boot 分区镜像。

## 发布前检查

发送给同学前建议提供整个目录的压缩包，同时保留 SHA-256。接收方解压后先读本文，再执行部署，避免遗漏 IQ 或刷错板型。

接收方可在目录根部执行 `sha256sum -c docs/SHA256SUMS`，一次检查 boot、程序、模型、IQ 和核心源码是否完整。
