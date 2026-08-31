# Jetson Orin NX 四路环视相机

本项目在 Jetson Orin NX 8GB 上采集 4 台 HIKROBOT MV-CB016-10UC-S，完成同步组帧、CUDA/NPP ISP、硬件编码，并将四路视频独立发送到接收电脑。

服务器接入和协议细节见 [`docs/SERVER_HANDOFF.md`](docs/SERVER_HANDOFF.md)。

## 当前实机配置

| 项目 | 当前值 |
|---|---|
| 相机 | 4× MV-CB016-10UC-S，共享 Realtek USB Hub / `tegra-xusb` |
| 采集 | 1440×1080、BayerRG8、60 FPS、Software Trigger |
| 同步 | FrameGroup 60 FPS |
| 下游输出 | 从 60 FPS 选择为 45 FPS |
| ISP | CUDA/NPP，输出 NV12 |
| 编码 | H.265、1440×1080@45 FPS、2 Mbps/路、GOP 30、无 B 帧 |
| 视频传输 | 4 路独立 MPEG-TS over SRT |
| SRT | Jetson caller，电脑 listener，latency 250 ms |
| Metadata | 独立 UDP 5100，SVMD v1，未改为 SRT |

当前网络地址：

- Jetson：`192.168.3.8`
- 接收电脑：`192.168.3.93`

视频端口映射：

| 相机 | SRT 端口 |
|---|---:|
| Front | 5000 |
| Rear | 5002 |
| Left | 5004 |
| Right | 5006 |

## 数据通路

```text
4× BayerRG8 1440×1080@60
  -> Software Trigger / FrameGroup @60
  -> 时间戳节拍选择 @45
  -> CUDA/NPP ISP (NV12)
  -> 4× nvv4l2h265enc @45, 2 Mbps/路
  -> appsink / 有界 latest queue / appsrc
  -> h265parse
  -> mpegtsmux alignment=7
  -> 4× srtsink caller

FrameGroup metadata
  -> UDP 5100 (SVMD v1)
```

采集核心仍使用固定 Serial Number 绑定、四个采集线程、Software Trigger、`FramePacket` 和有界 RingBuffer。鱼眼校正、BEV 与 360°拼接不属于当前 NX 发送端。

## 已固化的 USB 配置

四台相机共享同一 USB Hub/XUSB 时，必须保留以下配置：

```yaml
usb_transfer_size: 2097152
usb_transfer_ways: 1
sdk_image_nodes: 8
device_link_throughput_limit_bps: 140000
```

初始化顺序为：

```text
DeviceLinkThroughputLimitMode = On
DeviceLinkThroughputLimit = 140000
```

`device_link_throughput_limit_bps` 的名称为兼容现有配置而保留；实际数值范围和尺度以相机 GenICam 节点为准。MV-CB016-10UC-S 实测读回 140000 时可维持四路约 60 FPS。

## 构建

在 Jetson 上执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DMVS_SDK_LIB_DIR=/opt/MVS/lib/aarch64
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

部署前可检查 Jetson 多媒体环境：

```bash
./scripts/check_jetson_multimedia.sh
gst-inspect-1.0 nvv4l2h265enc
gst-inspect-1.0 h265parse
gst-inspect-1.0 mpegtsmux
gst-inspect-1.0 srtsink
```

CUDA/NPP ISP 和 Jetson 硬件编码是正式链路。`-DENABLE_CUDA_ISP=OFF` 仅用于非 Jetson 主机上的 CPU 正确性开发，不是生产 fallback。

## 启动流程

先在接收电脑启动四路 SRT listener：

```bash
cd /home/ubuntu/桌面/test/四路环视相机
./scripts/receive_four_grid_srt.sh
```

脚本优先选择 `nvh265dec`，然后依次尝试 `vah265dec` 和 `avdec_h265`。四宫格窗口为 1440×1080@60，布局为：

```text
Front | Rear
------+------
Left  | Right
```

编码流实际为 45 FPS；接收端 `videorate` 将显示输出维持在 60 FPS，因此新增画面仍只有 45 FPS。

随后在 Jetson 启动 caller：

```bash
cd /home/nvidia/four_camera_capture_test.4fo99V

./build-phase4/four_camera_capture \
  --config config/config.yaml \
  --encoder-cameras 4 \
  --output srt
```

正式构建目录可将可执行文件替换为 `./build/four_camera_capture`。不提供 `--duration` 时程序持续运行，按 `Ctrl+C` 正常释放相机和 GStreamer 资源。

如果启动时报相机打开失败 `0x80000203`，先检查是否已有旧进程占用相机：

```bash
pgrep -af four_camera_capture
```

## 运行时检查

正常状态下应重点关注：

- 四路 Capture 与 FrameGroup 输入约 60 FPS，FrameGroup 输出约 45 FPS。
- 四路 Encode 约 45 FPS、约 2 Mbps，`queue_drop=0`、`encoder_drop=0`、`errors=0`。
- 四路 Network `queue=0/2`、`drop=0`、`errors=0`。
- SRT `send-rate-mbps` 约 2，`bytes-sent-dropped=0`、`packets-sent-dropped=0`。
- SRT `negotiated-latency-ms=250`；Wi-Fi 抖动或丢包增大时需同时观察 RTT、NACK 和重传。
- Metadata `drop=0`、`errors=0`。

当前约 8 Mbps 的总目标视频码率适配现有 Wi-Fi，但 1440×1080@45 FPS、2 Mbps/路在大幅运动场景中会损失纹理细节。低码率不能补偿持续吞吐不足；若再次出现大量 `packets-sent-dropped`，应优先改善网络或使用千兆有线连接。

## 输出模式

`--output` 支持：

- `srt`：当前默认正式方案。H.264/H.265 使用 MPEG-TS；Jetson 为 caller。
- `rtp`：保留的 RTP/UDP 兼容方案，不是当前默认。
- `file`：保存裸编码流到 `encoded/`。
- `null`：编码后丢弃，用于隔离网络影响。

代码还保留 AV1 实验路径：Jetson 使用 `nvv4l2av1enc`，SRT 封装使用 streamable Matroska。当前 GStreamer 的 `mpegtsmux` 不支持该 AV1 输入，且 Matroska 在有字节缺口的 SRT 链路上可能导致 demux 致命错误，因此 AV1 不是当前交付方案。

原 RTP 四宫格脚本 `scripts/receive_four_grid.sh` 继续保留，不要与 SRT 脚本同时绑定相同端口。

## Metadata

Metadata 继续使用 UDP 5100，每个输出 FrameGroup 一包，SVMD v1 固定 136 字节，整数为 network byte order。查看数据：

```bash
python3 scripts/receive_metadata.py --bind 0.0.0.0 --port 5100
```

当前 SRT 四宫格脚本只负责预览，不消费 metadata，也不执行正式的四路时间戳关联。

## 主要文件

- `config/config.yaml`：采集、USB、ISP、编码、SRT 和 metadata 配置。
- `src/encoding/gstreamer_encoder.cpp`：Jetson 硬件编码 pipeline。
- `src/network/streamer.cpp`：null/file/RTP/SRT 输出。
- `src/network/metadata_sender.cpp`：SVMD v1 UDP 序列化与发送。
- `scripts/receive_four_grid_srt.sh`：电脑端 H.265/SRT 四宫格预览。
- `scripts/receive_four_grid.sh`：旧 RTP 四宫格预览。
- `docs/SERVER_HANDOFF.md`：服务端接入、协议和故障排查。
