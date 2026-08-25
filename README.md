# 四路环视相机 NX 发送端

Jetson Orin NX 8GB 上的四路 HIKROBOT USB 工业相机实时流水线：

正式服务器对接请先阅读 [`docs/SERVER_HANDOFF.md`](docs/SERVER_HANDOFF.md)，其中定义了
RTP 参数、SVMD v1 二进制协议、四路组帧算法、启停步骤和验收方法。

```text
4路 BayerRG8 采集 (100 FPS)
  -> Software Trigger / FrameGroup
  -> 时间戳节拍选择 (80 FPS)
  -> CUDA/NPP ISP (BayerRG8 -> NV12)
  -> Jetson 硬件 H.264/H.265 编码
  -> null / H.264文件 / RTP over UDP
  -> 独立 UDP FrameGroup metadata
```

采集核心保持原有设计：固定 Serial Number 绑定、四个采集线程、Software Trigger、
`FramePacket` 原有字段语义和 RingBuffer 覆盖策略未被重写。鱼眼校正、BEV 和 360°拼接不在 NX 发送端中。

> 当前实机通路与性能数据基于已稳定的 **640×480** 配置。最终目标 4×1440×1080@80
> 仍需在该分辨率下重新做通吐、USB 带宽、延迟和 30 分钟稳定性验收。

## 模块与背压

- `src/capture/`：`MultiCameraManager`、`FramePacket` 和每路固定容量 RingBuffer。
- `src/sync/`：100 Hz Software Trigger 调度与按 `trigger_cycle` 构成完整 `FrameGroup`。
- `src/processing/`：时间戳驱动的 100→80 FPS 选择器，以及 CUDA/NPP ISP。
- `src/encoding/`：每路独立的 GStreamer Jetson 硬件编码器。
- `src/network/`：`IStreamer`、null/file/RTP 输出和 metadata UDP 发送。
- `scripts/`：Jetson 环境检查、metadata 接收和 RTP/metadata 环回同步验证。

ISP、Encoder、Streamer 和 Metadata 队列都是固定容量；满时丢弃最旧项，不无界积压。
默认深度见 `config/config.yaml`，程序每秒汇总 selector/ISP/encoder/network 的帧率、丢帧、
队列、码率、阶段延迟和端到端延迟。

## Jetson 环境检查

不对 JetPack 版本或插件做静默假设，先运行：

```bash
./scripts/check_jetson_multimedia.sh
```

脚本检查 L4T/JetPack 信息、CUDA/NVCC/NPP、MVS SDK、GStreamer，以及
`nvvidconv`、`nvv4l2h264enc`、`nvv4l2h265enc` 等插件。缺失必需项时返回非零状态，
不会默认切换到软编码。

## 构建与测试

Jetson 上：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DMVS_SDK_LIB_DIR=/opt/MVS/lib/aarch64
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

CUDA/NPP ISP 默认为必需项。只有在非 Jetson 主机做 CPU 正确性开发时才显式使用
`-DENABLE_CUDA_ISP=OFF`；该模式不是四路 80 FPS 的生产 fallback。

## 发送端运行

配置文件为 `config/config.yaml`。可配置项包括分辨率、采集/输出 FPS、ISP 后端、
H.264/H.265、四路码率、GOP、队列深度、目标 IP、RTP 端口和 metadata 端口。

纯性能测试（编码结果丢弃）：

```bash
./build/four_camera_capture --duration 60 --encoder-cameras 4 --output null
```

本地保存裸 H.264：

```bash
./build/four_camera_capture --duration 10 --encoder-cameras 4 --output file
gst-launch-1.0 filesrc location=encoded/front.h264 ! h264parse ! fakesink
```

发送至当前配置的 `127.0.0.1`：

```bash
./build/four_camera_capture --duration 60 --encoder-cameras 4 --output rtp
```

`SIGINT`/`SIGTERM` 会按 Scheduler → ISP → Encoder → Streamer/Metadata → Camera 的顺序停止并释放资源。

## RTP 本机接收

默认映射：

| 相机 | RTP/UDP 端口 | RTP caps |
|---|---:|---|
| Front | 5000 | H.264, payload 96, 90 kHz |
| Rear | 5002 | H.264, payload 96, 90 kHz |
| Left | 5004 | H.264, payload 96, 90 kHz |
| Right | 5006 | H.264, payload 96, 90 kHz |

以 Front 为例，下面命令已在当前 Jetson 上验证，使用硬件解码并在无界面环境计算 FPS：

```bash
gst-launch-1.0 -v \
  udpsrc address=127.0.0.1 port=5000 \
    caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" \
  ! rtpjitterbuffer latency=20 drop-on-latency=true \
  ! rtph264depay ! h264parse ! nvv4l2decoder ! nvvidconv \
  ! fpsdisplaysink video-sink=fakesink text-overlay=false sync=false fps-update-interval=1000
```

验证其他相机时只替换 `port`。要在有图形环境的接收机显示画面，应将最后的
`fpsdisplaysink ...` 替换成该机器已验证的视频 sink；不在不同服务器环境间假定具体显示插件。

当前 Ubuntu/X11 接收机上已验证的四路 2×2 预览：

```bash
./scripts/receive_four_grid.sh
```

布局从左上到右下依次为 Front、Rear、Left、Right，输出窗口为 1280×960@25 FPS。

## Metadata 线格式与视频映射

Metadata 默认发往 UDP 5100，每个输出 `FrameGroup` 一包，固定 136 字节，
所有整数使用 network byte order：

| 字段 | 大小 | 说明 |
|---|---:|---|
| magic | 4 | `SVMD` / `0x53564D44` |
| version + packet_size | 2 + 2 | 当前版本 1，大小 136 |
| group_id | 8 | 同步组 ID |
| trigger_cycle | 8 | Software Trigger cycle |
| group_timestamp | 8 | 四帧共用 PTS，steady-clock ns |
| rtp_timestamp | 4 | `group_timestamp` 换算的 90 kHz 时间戳 |
| reserved | 4 | 保留，当前为 0 |
| 4×帧信息 | 4×24 | 每路 `frame_number/device_timestamp/host_timestamp` |

查看 metadata：

```bash
python3 scripts/receive_metadata.py --bind 127.0.0.1 --port 5100
```

可重复的四路映射验证（先运行接收器，再在另一终端启动 RTP 发送）：

```bash
python3 scripts/verify_loopback_sync.py --active-streams 4 --duration 16
./build/four_camera_capture --duration 8 --encoder-cameras 4 --output rtp
```

校验器只把 RTP marker packet 当作完整帧边界，再将其 RTP timestamp 与 metadata 中的
`rtp_timestamp` 逐组比对；它不会错把“第 N 个 UDP 包”当成“第 N 帧”。

## 当前实测结果（640×480）

- CUDA/NPP ISP：四路 80 group/s，平均约 0.76–0.80 ms/帧，P95 约 1.22–1.29 ms。
- 四路 H.264：每路约 79–80 FPS、约 20 Mbps，编码平均约 7–9 ms，P95 约 13–14 ms。
- 单路 RTP 本机硬解码：平均约 80.1 FPS，324 帧，0 丢帧。
- 四路 RTP + metadata：639 个 metadata 组，四路各 639 帧全部匹配，100%；网络队列丢弃 0，错误 0。
- 采集组时间戳到 RTP appsrc 成功接收的端到端延迟：四路平均 17.3–18.2 ms，P95 28.0–28.5 ms，最大 41.8 ms。
- 四路运行期间 ISP、Encoder 和 Network 未观察到持续积压，各级队列丢弃均为 0。

这些是短时功能/性能验收，不等价于 1440×1080 或 30 分钟稳定性验收。

## 当前内存复制路径

第一版不是 zero-copy，实际路径如下：

1. MVS SDK Bayer buffer 复制到预分配 host Bayer 池，然后立即归还 SDK buffer，避免悬空引用。
2. ISP 将 host Bayer 上传到 CUDA buffer，NPP/CUDA 在 device 内完成 demosaic、颜色参数和 NV12。
3. device NV12 复制回预分配 host NV12 池。
4. Encoder 将 host NV12 复制到 GStreamer buffer，`nvvidconv` 再转为 NVMM 供硬件编码。
5. 编码后 AU 从 appsink 复制到共享 byte vector；RTP 模式再复制到 payloader 的 GStreamer buffer。

`FramePacket`/`Nv12Frame`/`EncodedFrame` 的所有权由 `shared_ptr` 明确维持，各级接口未绑死底层存储，
后续可将 host NV12 池替换为 CUDA/NVMM 可互操作 buffer，以去掉 device→host→NVMM 往返复制。

CPU/GPU/RAM 可用 Jetson 自带工具与 null 模式同时采样：

```bash
tegrastats --interval 1000
./build/four_camera_capture --duration 60 --encoder-cameras 4 --output null
```
