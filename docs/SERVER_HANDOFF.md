# 四路环视视频发送/接收交接文档

文档版本：2.0

协议版本：SVMD v1

更新日期：2026-08-31

## 1. 当前交付基线

当前正式视频链路为四路独立 H.265/MPEG-TS over SRT，metadata 仍使用独立 UDP：

```text
Jetson Orin NX
  4× MV-CB016-10UC-S
  1440×1080 BayerRG8 @60
    -> Software Trigger / FrameGroup @60
    -> 时间戳选择 @45
    -> CUDA/NPP ISP (NV12)
    -> 4× nvv4l2h265enc @45, 2 Mbps/路, GOP 30
    -> h265parse -> mpegtsmux
    -> 4× SRT caller

  FrameGroup metadata -> UDP 5100 (SVMD v1)

接收电脑
  4× SRT listener
    -> tsdemux -> h265parse -> decoder
    -> videoconvert/videoscale/videorate
    -> compositor -> 1440×1080@60 preview
```

当前地址：

- Jetson：`192.168.3.8`
- 接收电脑：`192.168.3.93`

RTP/UDP 代码和旧接收脚本继续保留作兼容与诊断，不是当前默认方案。AV1 代码是实验能力，不属于当前交付基线。

## 2. 视频网络契约

### 2.1 连接角色和端口

Jetson 固定为 SRT caller，接收电脑固定为 listener：

| 逻辑流 | Jetson 目标 URI | 电脑监听 URI |
|---|---|---|
| Front | `srt://192.168.3.93:5000?mode=caller&latency=250` | `srt://0.0.0.0:5000?mode=listener&latency=250` |
| Rear | `srt://192.168.3.93:5002?mode=caller&latency=250` | `srt://0.0.0.0:5002?mode=listener&latency=250` |
| Left | `srt://192.168.3.93:5004?mode=caller&latency=250` | `srt://0.0.0.0:5004?mode=listener&latency=250` |
| Right | `srt://192.168.3.93:5006?mode=caller&latency=250` | `srt://0.0.0.0:5006?mode=listener&latency=250` |
| Metadata | UDP 目标 `192.168.3.93:5100` | UDP 5100 |

防火墙需允许 UDP 5000、5002、5004、5006 和 5100。SRT 建立在 UDP 上；不要按 TCP 端口放行。

### 2.2 当前编码参数

| 项目 | 值 |
|---|---|
| Codec | H.265/HEVC |
| 输入尺寸 | 1440×1080 |
| 编码帧率 | 45 FPS |
| 码率 | 2,000,000 bit/s/路，四路目标总计约 8 Mbps |
| GOP / IDR interval | 30 帧，约 0.67 秒 |
| B 帧 | 0 |
| 码流 | Annex-B byte-stream，AU 对齐 |
| 封装 | MPEG-TS，`mpegtsmux alignment=7` |
| SRT latency | 250 ms，发送与接收两端一致 |

相机采集和 FrameGroup 仍为 60 FPS。45 FPS 只作用于下游选择、编码和视频传输，不改变相机触发频率。

### 2.3 Pipeline

Jetson 每路发送 pipeline 的关键段为：

```text
nvv4l2h265enc
  bitrate=2000000
  iframeinterval=30
  idrinterval=30
  insert-sps-pps=true
  num-B-Frames=0
-> h265parse config-interval=-1
-> appsink max-buffers=2 drop=true sync=false
-> 程序有界 latest queue
-> appsrc block=false caps=video/x-h265,stream-format=byte-stream,alignment=au
-> queue max-size-buffers=2 leaky=downstream
-> h265parse config-interval=-1
-> mpegtsmux alignment=7
-> srtsink mode=caller latency=250
   wait-for-connection=false sync=false async=false
```

接收端每路关键段为：

```text
srtsrc mode=listener latency=250
-> tsdemux
-> h265parse
-> nvh265dec（fallback: vah265dec / avdec_h265）
-> videoconvert -> videoscale -> videorate
-> video/x-raw,format=I420,width=1440,height=1080,framerate=60/1
-> queue max-size-buffers=4 leaky=downstream
-> compositor
```

四路 raw caps 在进入 compositor 前显式统一为普通 `video/x-raw` I420。最终窗口 1440×1080@60，每个 tile 为 720×540；流中实际新增画面为 45 FPS，`videorate` 只负责匹配显示输出速率。

## 3. USB 采集约束

四台相机共享 Realtek USB Hub 和 Jetson `tegra-xusb`。以下配置已经实机验证，不应随网络调优一起修改：

```yaml
width: 1440
height: 1080
pixel_format: BayerRG8
frame_rate: 60

usb_transfer_size: 2097152
usb_transfer_ways: 1
sdk_image_nodes: 8
device_link_throughput_limit_bps: 140000
```

相机打开后、开始采集前，初始化必须保持：

```text
DeviceLinkThroughputLimitMode = On
DeviceLinkThroughputLimit = 140000
```

成功日志至少应包含：

```text
throughput_limit_mode_set_ret=0x0
throughput_limit_requested=140000
throughput_limit_readback=140000
throughput_limit_set_ret=0x0
throughput_limit_read_ret=0x0
```

字段名中的 `bps` 为兼容已有配置而保留，实际数值尺度以相机 GenICam 节点为准。

## 4. 配置与启动

### 4.1 Jetson 配置

关键配置为：

```yaml
four_camera_capture:
  processing:
    output_fps: 45

  encoder:
    enabled: true
    codec: h265
    fps: 45
    bitrate_front: 2000000
    bitrate_rear: 2000000
    bitrate_left: 2000000
    bitrate_right: 2000000
    gop: 30
    low_latency: true
    active_cameras: 4
    queue_depth: 2

  output:
    mode: srt
    queue_depth: 2

  network:
    server_ip: "192.168.3.93"
    srt_latency_ms: 250
    metadata_enabled: true
    metadata_port: 5100
    metadata_queue_depth: 4

  streams:
    front_port: 5000
    rear_port: 5002
    left_port: 5004
    right_port: 5006
```

程序只在启动时读取配置。修改 IP、码率、帧率或 SRT latency 后必须重启发送端；SRT latency 还必须同步修改接收端 URI。

### 4.2 插件检查

Jetson：

```bash
gst-inspect-1.0 nvv4l2h265enc
gst-inspect-1.0 h265parse
gst-inspect-1.0 mpegtsmux
gst-inspect-1.0 srtsink
```

接收电脑：

```bash
gst-inspect-1.0 srtsrc
gst-inspect-1.0 tsdemux
gst-inspect-1.0 h265parse
gst-inspect-1.0 nvh265dec
```

如果没有 `nvh265dec`，仓库脚本会尝试 VA-API 或 libav 软件解码器。

### 4.3 构建

在 Jetson 上：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DMVS_SDK_LIB_DIR=/opt/MVS/lib/aarch64
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

测试设备当前也保留 `build-phase4`。正式交付应使用统一的 `build` 目录。

### 4.4 启动顺序

先在电脑启动 listener：

```bash
cd /home/ubuntu/桌面/test/四路环视相机
./scripts/receive_four_grid_srt.sh
```

再在 Jetson 启动 caller：

```bash
cd /home/nvidia/four_camera_capture_test.4fo99V

./build-phase4/four_camera_capture \
  --config config/config.yaml \
  --encoder-cameras 4 \
  --output srt
```

正式构建目录使用：

```bash
./build/four_camera_capture \
  --config config/config.yaml \
  --encoder-cameras 4 \
  --output srt
```

停止时先在 Jetson 按 `Ctrl+C`，等待相机和 GStreamer 正常释放，再停止接收端。不要使用 `kill -9`。

## 5. 运行时验收

### 5.1 Jetson 日志

正常运行时关注：

- Capture：四路约 60 FPS，启动瞬态后 `invalid` 不应持续增长。
- FrameGroup：输入约 60 FPS，输出约 45 FPS。
- ISP：约 45 group/s，`isp_drop=0`、`errors=0`。
- Encode：四路约 45 FPS、约 2 Mbps，`queue_drop=0`、`encoder_drop=0`、`errors=0`。
- Network：四路 `queue=0/2`、`drop=0`、`errors=0`。
- Metadata：随输出 FrameGroup 增长，`drop=0`、`errors=0`。

SRT 原生统计至少应满足：

```text
negotiated-latency-ms=(int)250
send-rate-mbps≈2
bytes-sent-dropped=(guint64)0
packets-sent-dropped=(int)0
```

少量 `packets-sent-lost` 在已成功 retransmit 且 sent-dropped 仍为 0 时不等于画面已经损坏。持续增长的 NACK、RTT、重传或 sent-dropped 表示网络无法在 latency 窗口内恢复。

### 5.2 接收端

至少确认：

1. 四路均能建立 SRT 连接并持续解码。
2. Front/Rear/Left/Right 映射正确。
3. 无规则竖向拉伸、错行或块状内存错位。
4. 无 ERROR/EOS，接收进程持续运行。
5. 快速运动时没有因丢失参考帧造成持续马赛克。

当前 2 Mbps/路是为现有 Wi-Fi 吞吐和时延做出的取舍。静态画面通常清晰，快速运动区域的纹理和边缘会比原 20 Mbps 配置更软。若需要同时提高画质和稳定性，应优先使用千兆有线网络，而不是继续增大 SRT latency。

## 6. SVMD v1 Metadata

Metadata 本次未迁移到 SRT，仍由 Jetson 发往 UDP 5100。每个输出 FrameGroup 一包，固定 136 字节，所有多字节整数使用 network byte order。

### 6.1 头部

| 字节偏移 | 大小 | 类型 | 字段 | 说明 |
|---:|---:|---|---|---|
| 0 | 4 | uint32 | magic | `0x53564D44`，ASCII `SVMD` |
| 4 | 2 | uint16 | version | 当前为 1 |
| 6 | 2 | uint16 | packet_size | 当前为 136 |
| 8 | 8 | uint64 | group_id | 当前实现等于 `trigger_cycle` |
| 16 | 8 | uint64 | trigger_cycle | Software Trigger 周期号 |
| 24 | 8 | int64 | group_timestamp | Jetson steady-clock ns |
| 32 | 4 | uint32 | rtp_timestamp | 兼容字段，由 group timestamp 换算到 90 kHz |
| 36 | 4 | uint32 | reserved | 当前为 0 |

`group_timestamp` 不是 Unix epoch，不能直接与接收电脑的系统时间相减。

90 kHz 换算规则：

```text
seconds   = group_timestamp_ns / 1_000_000_000
remainder = group_timestamp_ns % 1_000_000_000
rtp_timestamp = (seconds * 90_000
                 + remainder * 90_000 / 1_000_000_000) mod 2^32
```

### 6.2 四路帧记录

偏移 40 开始按 Front、Rear、Left、Right 固定放置 4 个 24 字节记录：

| 相机 | 记录偏移 | frame_number | device_timestamp | host_timestamp |
|---|---:|---:|---:|---:|
| Front | 40 | 40 | 48 | 56 |
| Rear | 64 | 64 | 72 | 80 |
| Left | 88 | 88 | 96 | 104 |
| Right | 112 | 112 | 120 | 128 |

每个记录由三个 64 位字段组成：

```text
uint64 frame_number
uint64 device_timestamp
int64  host_timestamp
```

接收端至少检查 payload 长度、magic、version 和 packet_size；未知版本应丢弃，不应按 v1 强制解析。

当前 `receive_four_grid_srt.sh` 只做视频预览，不消费 metadata。旧的 `verify_loopback_sync.py` 验证的是 RTP marker/timestamp 与 metadata 的映射，不应直接当作 SRT/MPEG-TS 正式组帧验收。正式服务器若需要在 SRT 链路恢复四路 FrameGroup，必须先验证 MPEG-TS PTS 保留规则，并为解码帧保留可追踪的时间戳 side data。

查看 metadata：

```bash
python3 scripts/receive_metadata.py --bind 0.0.0.0 --port 5100
```

## 7. 兼容输出

程序支持：

```text
--output null
--output file
--output rtp
--output srt
```

- `srt`：当前默认。
- `rtp`：保留的 H.264/H.265 RTP/UDP 路径。
- `file`：保存裸编码流。
- `null`：编码后丢弃，用于隔离网络问题。

AV1 实验路径使用 `nvv4l2av1enc`。当前 Jetson GStreamer 的 `mpegtsmux` 不接受 AV1，因此 SRT AV1 使用 streamable Matroska；实测 Matroska 在有字节缺口时可能报 large block/corrupt stream 并终止 demux。当前网络交付应保持 H.265/MPEG-TS。

## 8. 故障排查

### SRT 无连接或无画面

1. 确认电脑 listener 已先启动。
2. 检查 `server_ip` 是否为接收电脑当前 IPv4。
3. 用 `ip route get 192.168.3.93` 检查 Jetson 路由。
4. 检查 UDP 5000/5002/5004/5006 防火墙规则。
5. 检查 `srtsink`/`srtsrc`、`mpegtsmux`/`tsdemux` 与 H.265 插件。

### 画面卡顿或运动时马赛克

先看 SRT 原生统计，不要先修改相机、USB、ISP 或编码架构：

- `packets-sent-dropped` / `bytes-sent-dropped` 增长：吞吐不足或数据超过 SRT latency 窗口。
- RTT/NACK/重传剧增：Wi-Fi 抖动或丢包。
- 发送端 Network queue drop 增长：应用层无法及时送入 SRT。
- 所有发送统计正常但单路仍损坏：再隔离单路 `srtsrc -> tsdemux -> h265parse -> decoder -> sink`。

当前 250 ms latency 基于约 2 Mbps/路且低 RTT 的现场状态。若网络环境恶化，可同时提高发送端与接收端 latency；只改一端会使实际协商结果难以判断。

### 延迟较大

编码阶段通常只有毫秒级延迟。端到端约 2～3 秒时首先检查 `negotiated-latency-ms` 是否仍为旧的 2000，而不是先归因于编码器。修改配置和脚本后必须重启两端。

### 相机打开失败 `0x80000203`

通常表示相机仍被旧进程占用：

```bash
pgrep -af four_camera_capture
```

先正常停止旧进程，再重新启动；不要反复并行启动多个采集程序。

### 四宫格格式异常

当前脚本在进入 compositor 前将四路统一为 system-memory I420，并为每路设置独立 leaky queue。不要绕过显式 raw caps，或直接把不同 GPU memory/caps 的解码输出混入 compositor。

## 9. 关联文件

- `config/config.yaml`：当前采集、USB、编码和网络基线。
- `src/encoding/gstreamer_encoder.cpp`：H.264/H.265/AV1 Jetson 编码 pipeline。
- `src/network/streamer.cpp`：null/file/RTP/SRT 输出与 SRT 统计。
- `src/network/metadata_sender.cpp`：SVMD v1 发送。
- `scripts/receive_four_grid_srt.sh`：当前 H.265/SRT 四宫格接收。
- `scripts/receive_four_grid.sh`：旧 RTP 四宫格接收。
- `scripts/receive_metadata.py`：metadata 参考解析器。
- `scripts/verify_loopback_sync.py`：旧 RTP/metadata 映射验证器。
- `README.md`：项目总览与启动流程。
