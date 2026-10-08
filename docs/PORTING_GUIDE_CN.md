# 迁移和启动指南

## 前置条件

目标板应为 RK3588 AArch64 Linux，并具有两颗 IMX415、RKNN 驱动 `/dev/rknpu`、RKISP/RKCIF、`v4l2-ctl`、`media-ctl` 和 `rkaiq_3A_server`。显示模式还需要 Wayland、Python 3、OpenCV (`cv2`) 与 NumPy。

高速 ROI 还依赖 `runtime/iqfiles` 中增加了 640×640 LSC 条目的 IMX415 IQ 文件。若仍使用只含 3840×2160 条目的原始 IQ，CSI/CIF RAW 数据虽正常，RKISP 的 ROI NV12 输出会是纯黑。`deploy-adb.sh` 会备份原文件并安装已验证版本。

当前二进制只适配已验证拓扑：CSI3 为 `/dev/media4`、`/dev/v4l-subdev2`、`/dev/video44`；CSI4 为 `/dev/media5`、`/dev/v4l-subdev7`、`/dev/video53`。新板接口或镜像不同，先执行 `media-ctl -p -d /dev/mediaX` 比对实体，不要直接修改随机节点尝试。

## ADB 部署

在主机解压包后执行：

```sh
./deploy-adb.sh
```

多设备时：

```sh
./deploy-adb.sh 设备序列号
```

也可手工部署：

```sh
adb push runtime/. /userdata/dual-person-demo/
adb shell 'chmod +x /userdata/dual-person-demo/*.sh /userdata/dual-person-demo/imx415_dual_person_demo'
```

## 板端检查和启动

```sh
adb shell
cd /userdata/dual-person-demo
./preflight.sh
./run.sh
```

无 HDMI/无 Python OpenCV 时可只运行检测：

```sh
NO_DISPLAY=1 ./run.sh
```

参数示例：

```sh
THRESHOLD=0.25 GLOBAL_PERIOD_MS=100 \
ROI_SCALE_X=1.0 ROI_SCALE_Y=1.0 \
ROI_OFFSET_X=0 ROI_OFFSET_Y=0 ROI_MOVE_THRESHOLD=48 \
./run.sh
```

停止与日志：

```sh
./stop.sh
tail -f /tmp/dual-person-demo.jsonl
cat /tmp/dual-person-hdmi.log
cat /tmp/rkaiq_dual_global_roi.log
dmesg | grep -Ei 'csi|mipi|crc|ecc|overflow|size'
```

## 驱动迁移

`source/driver/imx415.c` 是来源 BSP 的参考实现，不是可加载的用户态文件。目标板 BSP 若不同，应把以下功能作为独立补丁合入并重新构建 kernel/module 和 DTB：

- 648×640 RAW10 高速 ROI mode table；
- `VIDIOC_SUBDEV_S_SELECTION` crop 支持；
- 流运行时用 group-hold 写入 `PIX_HST/PIX_VST`；
- 与目标 DPHY/CSI 接口对应的 lane、link frequency、pixel rate 和设备树端点。

烧写前保留原镜像/DTB/模块，先在单路相机验证 RAW 流，再启用 ISP 和双路运行。

## 验收建议

1. `preflight.sh` 全部通过。
2. 分别对 `/dev/video44` 和 `/dev/video53` 独立抓流，确认约 60 FPS 和约 110 FPS。
3. 双路同时抓流 10 分钟，内核日志无 CRC/ECC/overflow/size error。
4. 启动 Demo，无人时全局推理接近 60 FPS；出现 person 后 ROI 激活且动态窗口能跟随。
5. 校正 `ROI_SCALE_*`/`ROI_OFFSET_*` 后，在多个距离和画面边缘验证目标仍位于 ROI 内。
