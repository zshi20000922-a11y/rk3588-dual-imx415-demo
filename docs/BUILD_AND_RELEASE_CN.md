# 构建与发布指南

## 1. 所需基础工程

交接包只保存项目修改层。重新构建需要：

- 完整 Rockchip `rknn_model_zoo`；
- RK3588 Linux AArch64 交叉编译器；
- RKNN runtime 和头文件；
- RGA/imageutils/fileutils 源码（model zoo 内已有）；
- Linux 6.1 BSP（仅重新构建驱动/boot 时需要）。

已验证的本机基础目录是：

```text
/home/user/PycharmProjects/shize/RK3588/rknn_model_zoo-main
/home/user/PycharmProjects/shize/RK3588/04、linux6.1_sdk/clean-sdk-20260918/atk_dlrk3588_linux6.1
```

## 2. 应用源码覆盖方式

假设完整 model zoo 位于 `$MODEL_ZOO`，将文件复制到 YOLOv5 C++ 示例目录：

```bash
cp source/application/imx415-dual-person-demo.cc \
   source/application/imx415-yolo-roi-demo.cc \
   "$MODEL_ZOO/examples/yolov5/cpp/"

cp source/rknn_overlay/CMakeLists.txt \
   "$MODEL_ZOO/examples/yolov5/cpp/CMakeLists.txt"

cp source/rknn_overlay/rknpu2/yolov5.cc \
   "$MODEL_ZOO/examples/yolov5/cpp/rknpu2/yolov5.cc"
```

覆盖前应先保存基础工程版本或创建 Git 分支。不同版本 model zoo 的 API 可能变化，不应盲目覆盖后忽略编译警告。

## 3. 应用构建

使用 model zoo 提供的 RK3588 Linux AArch64 构建方式。当前主机产物目录为：

```text
rknn_model_zoo-main/build/dual-person-rk3588/
```

目标产物：

```text
imx415_dual_person_demo
```

构建后检查：

```bash
file imx415_dual_person_demo
readelf -d imx415_dual_person_demo | grep NEEDED
```

必须是 AArch64 Linux ELF。运行时至少依赖 `librknnrt.so` 和标准 C/C++ 运行库；RGA 可能由 imageutils 动态加载。

## 4. 内核驱动构建

将 `source/kernel/imx415.c` 与目标 BSP 同路径驱动比较/合入：

```text
kernel-6.1/drivers/media/i2c/imx415.c
```

当前使用：

```text
RK_DEFCONFIG=01_atk_dlrk3588_auto2mipi_2hdmi_defconfig
RK_KERNEL_CFG=alientek_rk3588_defconfig
RK_KERNEL_DTB=rk3588-alientek-2mipi720x1280-2hdmi.dtb
```

需要重点审查：

- 648×640 RAW10 mode table；
- VMAX/HMAX、link frequency、pixel rate 和 4 lane 配置；
- `VIDIOC_SUBDEV_S_SELECTION`；
- group-hold 更新 `PIX_HST/PIX_VST`；
- CSI3/CSI4 对应设备树端点和 I²C bus。

构建出的 boot 应包含 kernel、DTB 和 resource。不要把单个 `imx415.c` 复制到板端期待生效。

## 5. IQ 发布

板端目标：

```text
/etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json
```

发布 `iq/imx415-roi-640x640.json` 前先备份目标板文件。验证 JSON 中存在：

```text
lsc_v2.common.resolutionAll[].name == "640x640"
```

若缺少该条目，典型现象是 `/dev/video33` RAW 有数据，而 `/dev/video53` NV12 为 Y=0、UV=128。

## 6. runtime 发布结构

板端标准目录应为：

```text
/userdata/dual-person-demo/
```

保持 `runtime/` 内相对结构不变。主程序从工作目录读取：

```text
model/yolov5n.rknn
model/coco_80_labels_list.txt
```

因此必须通过 `run.sh` 或先 `cd` 到该目录运行。

## 7. 发布前验收

1. `preflight.sh` 通过；
2. CSI3 `/dev/video44` 双路并发时约 60 FPS；
3. CSI4 `/dev/video53` 双路并发时约 110 FPS；
4. ROI NV12 的 Y 平面不是全零；
5. NPU 静态图推理正确；
6. 无人时全局推理约 60 FPS；
7. 出现 person 后 ROI PiP 显示、动态 selection 移动、ROI 推理约 97～101 FPS；
8. `dmesg` 无 CRC/ECC/overflow/size error；
9. 运行至少 30 分钟并检查温度、降频和内存。

## 8. 版本信息建议

每次交付建议记录：

- Git commit 或源码目录 SHA-256；
- boot.img SHA-256；
- IQ JSON SHA-256；
- RKNN 模型 SHA-256；
- `uname -a`、RKNPU driver、RKNN runtime 和 RGA 版本；
- 摄像头接口与实际 I²C/media/video 映射；
- 现场性能和已知问题。
