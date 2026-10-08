# RK3588 双 IMX415：全局搜索 + 高速 ROI 人体检测 Demo

## 数据通路

- CSI3 `/dev/video44`：IMX415 以 3840×2160、60 FPS 读出，RKISP 缩放到 NV12 960×540；YOLOv5n 每 500 ms 做一次全局 person 搜索。
- CSI4 `/dev/video53`：IMX415 直接使用 648×640 RAW10 传感器 ROI，约 110.45 FPS；RKISP 中心裁成 NV12 640×480；主摄发现 person 后才启动该路推理并显示画中画。
- 两颗相机不做双目标定。全局路和 ROI 路只共享“是否发现 person”的语义，不把像素坐标当作精确的跨相机坐标。

高速 ROI 不是从 4K 图像软件裁剪得到；CSI4 在设置 subdev format 时已经切换到传感器 ROI 模式。

## 板端运行

```sh
/userdata/dual-person-demo/run.sh
```

默认持续运行，按 Ctrl-C 退出。可通过环境变量调整：

```sh
DURATION=30 GLOBAL_PERIOD_MS=300 THRESHOLD=0.25 \
  /userdata/dual-person-demo/run.sh
```

结构化结果写入 `/tmp/dual-person-demo.jsonl`：

- `global_camera_fps`、`roi_camera_fps`：两路实际采集帧率；
- `global_infer_fps`、`roi_infer_fps`：两路实际 YOLO 推理吞吐；
- `global_person`、`roi_person`：对应相机是否检测到人；
- `global_confidence`、`roi_confidence`：最高 person 置信度。

## 当前实测

15 秒无人场景稳定性测试：

- 全局相机约 60.0 FPS；
- ROI 相机约 109.5～111.9 FPS；
- NPU0 周期执行全局搜索；ROI 激活后 NPU0/NPU1/NPU2 领取不同的最新帧并行推理；
- ROI YOLOv5n 三核聚合检测约 90～92 FPS；
- HDMI 默认只显示全局主画面；发现 person 后在右下角显示带检测框的高速 ROI 画中画，目标消失约 2 秒后自动隐藏。

模型准确性另用 COCO `bus.jpg` 验证：YOLOv5n 正确输出 3 个 person（最高置信度 0.810）和 1 个 bus；因此不是只验证 RKNN API 返回成功。

110 FPS 是 ROI 图像采集速率，不应误写为当前 AI 推理速率。程序始终取最新帧，所以推理较慢时不会积累旧帧延迟。

## 动态 ROI 映射

主摄 person 框中心会按归一化坐标映射到副摄 IMX415 的 3840×2160 可见坐标，再通过运行中的 `VIDIOC_SUBDEV_S_SELECTION` 更新副摄 640×640 ROI 起点。驱动使用 group-hold 同步写入位置寄存器，不需要停流。

两颗相机没有做双目标定，默认使用同尺度映射。可按现场安装角度校正：

```sh
ROI_SCALE_X=1.0 ROI_SCALE_Y=1.0 \
ROI_OFFSET_X=0 ROI_OFFSET_Y=0 \
ROI_MOVE_THRESHOLD=48 \
  /userdata/dual-person-demo/run.sh
```

偏移量单位是副摄全分辨率像素。移动死区用于避免检测框轻微抖动导致传感器 ROI 每帧移动。

## 低拷贝优化

- NV12 不再由 CPU 逐像素转换成 RGB；直接交给 RGA 完成颜色转换和 letterbox。
- 每个 NPU 工作线程复用 640×640 输入缓冲，不再逐帧 `malloc/free`。
- 三个 NPU 工作线程通过原子序号领取不同 ROI 帧，避免重复推理同一帧。
- 实测三核 ROI 聚合吞吐约 100～106.5 FPS，接近约 110 FPS 的输入上限。
