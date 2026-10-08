# 版本与完整性校验

## 交付基线

- 整理日期：2026-09-20
- 目标平台：RK3588 / AArch64
- 内核基线：Linux 6.1.141
- 摄像头：双 IMX415，CSI3 为全局相机，CSI4 为高速 ROI 相机
- 模型：YOLOv5n RKNN，640×640 输入，业务筛选 `person`

## 关键文件 SHA-256

解压后可在交接包根目录执行：

```bash
sha256sum -c docs/SHA256SUMS
```

| 文件 | SHA-256 |
|---|---|
| `firmware/boot-linux6.1.141-dual-mipi-roi.img` | `303e6b2cc77b9a2bc5be2808d09dd392fedbf9010dadbdaddbe8f9be237eb269` |
| `runtime/imx415_dual_person_demo` | `b85c33f508a4fd42ada67165660d63faea08b56530971fce3b49a8fea7e9ad18` |
| `runtime/model/yolov5n.rknn` | `6e16af92fb62775adbc226c6f6fb32a1d577097ebde879693161e7c7262f8579` |
| `runtime/dual-person-hdmi.py` | `78b35c0dcc610a8121d00d1e72b051632316b81ad41e5ad710994e98894436cf` |
| `runtime/configure-cameras.sh` | `f8d6df54ea4ec713b4b5048c8755e8e466b1f20939b89cb600b6477094727291` |
| `iq/imx415-roi-640x640.json` | `b19167ecca26c9e047f3ccefa4d9cd4051a25c38f5e6ec44e2042cf664e6504e` |
| `source/application/imx415-dual-person-demo.cc` | `744b53327910060c0e0a87ca5f229a3621dc49ef7d9b4659f1928a237a95c8ac` |
| `release/dual-person-demo-portable-20260920.tar.gz` | `fc491707401b7cee00541706665213b2661da14ddb300d2681a110d5d4e96b09` |

这些摘要用于确认传输完整，并不表示 boot 镜像适用于任意 RK3588 产品。刷写前仍须核对板型、分区布局、设备树和摄像头接口。
