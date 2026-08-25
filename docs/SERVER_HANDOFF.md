# 四路环视视频发送/服务器接收交接文档

文档版本：1.0

协议版本：SVMD v1

更新日期：2026-08-25

## 1. 范围与当前状态

Jetson Orin NX 发送端的数据通路为：

```text
4路 BayerRG8 采集
  -> Software Trigger / FrameGroup
  -> 100 FPS 时间戳选择为 80 FPS
  -> CUDA/NPP ISP (NV12)
  -> Jetson 硬件 H.264
  -> 4路独立 RTP/UDP
  + 1路 FrameGroup metadata UDP
```

服务器后续可进行解码、鱼眼校正、BEV 和拼接，但这些算法不属于当前 NX 仓库。

当前已验证配置是 **4×640×480@80 FPS**，每路约 20 Mbps。
4×1440×1080@80 FPS 是最终目标，但尚未完成该分辨率的长时稳定性验收。

## 2. 网络契约

默认目的端口如下。服务器应绑定 `0.0.0.0`或指定的本地网卡 IP，
不应依赖 Jetson 的 UDP 源端口，因为源端口是动态的。

| 逻辑流 | 目的 UDP 端口 | 内容 |
|---|---:|---|
| Front | 5000 | H.264 RTP |
| Rear | 5002 | H.264 RTP |
| Left | 5004 | H.264 RTP |
| Right | 5006 | H.264 RTP |
| FrameGroup metadata | 5100 | 136 字节 SVMD v1 |

网络层特性：

- 视频使用 RTP over UDP，没有 RTSP、SDP 服务或 RTCP 控制面。
- Metadata 使用独立的原始 UDP 数据报。
- 当前不提供重传、加密、认证或拥塞控制，建议使用可控局域网。
- 默认总视频码率约 80 Mbps，加上 RTP/UDP/IP 开销后应预留更多带宽；正式部署建议千兆有线网络。
- 服务器防火墙需允许 UDP 5000、5002、5004、5006、5100。

## 3. RTP 视频协议

### 3.1 默认 H.264 参数

| 项目 | 值 |
|---|---|
| RTP media | `video` |
| encoding-name | `H264` |
| payload type | 96（可配置） |
| RTP clock rate | 90000 Hz |
| RTP MTU | 1400 字节（可配置） |
| H.264 stream format | byte-stream / Annex B |
| H.264 alignment | Access Unit |
| 默认帧率 | 80 FPS |
| 默认 GOP | 40 帧（约 0.5 s） |
| B 帧 | 0 |
| SPS/PPS | 编码器插入，RTP payloader 每秒周期发送 |
| 默认码率 | 每路 20,000,000 bit/s |

服务器必须为 UDP 输入显式提供 RTP caps：

```text
application/x-rtp,
media=video,
encoding-name=H264,
payload=96,
clock-rate=90000
```

发送端为每个编码 Access Unit 设置 PTS，四个属于同一 `FrameGroup` 的帧使用相同 PTS。
RTP payloader 把该 PTS 换算为 90 kHz RTP timestamp。

一帧 H.264 通常被拆成多个 UDP/RTP 包：

- 同一帧的所有 RTP 包共用同一 RTP timestamp。
- 该帧最后一个 RTP 包的 Marker bit 为 1。
- 不能把“第 N 个 UDP 包”视为“第 N 帧”。
- RTP sequence number 只用于单路内的丢包/乱序检查，不用于四路组帧。
- SSRC 由每路 RTP 会话产生，服务器应以目的端口和实际 SSRC 管理流，不要写死 SSRC。

RTP timestamp 是 32 位无符号值，约每 13.26 小时回绕一次。
缓存、排序和比较代码必须按 32 位模算术处理回绕。

### 3.2 H.265 扩展

发送端保留 `codec: h265` 配置和 Jetson `nvv4l2h265enc`/`rtph265pay` 路径。
切换后 `encoding-name` 为 `H265`，服务器应使用 H.265 depay/parser/decoder。
H.265 不是当前局域网验收基线；未经重新验收不应直接在生产系统启用。

## 4. SVMD v1 Metadata 协议

### 4.1 传输规则

- 默认目的 UDP 端口：5100。
- 每个选中的 `FrameGroup` 发送一个 metadata 数据报。
- 数据报固定为 136 字节，不是裸 C/C++ struct。
- 所有多字节整数使用 network byte order（big-endian）。
- 有符号 64 位字段使用二进制补码的 big-endian 表示。
- UDP 可丢包、乱序或重复；接收端必须允许 metadata 比视频先到或后到。

### 4.2 头部字段

| 字节偏移 | 大小 | 类型 | 字段 | 说明 |
|---:|---:|---|---|---|
| 0 | 4 | uint32 | magic | `0x53564D44`，ASCII `SVMD` |
| 4 | 2 | uint16 | version | 当前为 1 |
| 6 | 2 | uint16 | packet_size | 当前为 136 |
| 8 | 8 | uint64 | group_id | 组 ID；当前实现等于 `trigger_cycle` |
| 16 | 8 | uint64 | trigger_cycle | Software Trigger 周期号 |
| 24 | 8 | int64 | group_timestamp | 同步组时间戳，Jetson steady-clock ns |
| 32 | 4 | uint32 | rtp_timestamp | 用于视频帧映射的 90 kHz RTP timestamp |
| 36 | 4 | uint32 | reserved | 当前必须为 0；接收端应忽略未来的非零值 |

`group_timestamp` 是该组四帧 `host_timestamp` 的最大值，表示四帧已齐备的时刻。
steady clock 不是 Unix epoch，不能直接转换为日历时间，也不能与服务器本机时钟直接相减。
它可用于 Jetson 单次启动内的顺序、间隔和 RTP timestamp 还原。

90 kHz 换算规则是整数向下取整，最终保留低 32 位：

```text
seconds   = group_timestamp_ns / 1_000_000_000
remainder = group_timestamp_ns % 1_000_000_000
rtp_timestamp = (seconds * 90_000
                 + remainder * 90_000 / 1_000_000_000) mod 2^32
```

### 4.3 四路帧字段

偏移 40 开始固定按 `Front, Rear, Left, Right` 顺序放置 4 个 24 字节记录：

| 相机 | 记录偏移 | frame_number | device_timestamp | host_timestamp |
|---|---:|---:|---:|---:|
| Front | 40 | 40 | 48 | 56 |
| Rear | 64 | 64 | 72 | 80 |
| Left | 88 | 88 | 96 | 104 |
| Right | 112 | 112 | 120 | 128 |

每个记录的字段为：

| 记录内偏移 | 大小 | 类型 | 字段 |
|---:|---:|---|---|
| 0 | 8 | uint64 | frame_number |
| 8 | 8 | uint64 | device_timestamp |
| 16 | 8 | int64 | host_timestamp（Jetson steady-clock ns） |

`device_timestamp` 是相机/MVS SDK 原始值。未做时钟单位或跨相机校正的协议承诺，
服务器不应仅依赖它来恢复四路组帧。

### 4.4 协议校验

接收端至少必须检查：

```text
UDP payload length == 136
magic == 0x53564D44
version == 1
packet_size == 136
```

对未知 version 不要按 v1 强行解析；记录错误后丢弃该包。

## 5. 服务器恢复四路 FrameGroup

### 5.1 映射主键

**视频帧与 metadata 的唯一标准映射键是 32 位 `rtp_timestamp`。**

四个属于同一 `FrameGroup` 的视频帧，在四路 RTP 上使用同一 RTP timestamp；
metadata 包中的 `rtp_timestamp` 也是该值。

不要使用以下方法组帧：

- UDP 包到达顺序。
- 每路第 N 个 RTP 包或第 N 帧。
- 服务器的接收时间“差不多”匹配。
- 不同 RTP 流的 sequence number。

### 5.2 推荐接收状态机

```text
metadata UDP -> validate -> metadata_by_rtp_ts[rtp_timestamp]

Front RTP -> jitter/reorder -> depayload/decode -> frame_by_ts[Front][rtp_timestamp]
Rear  RTP -> jitter/reorder -> depayload/decode -> frame_by_ts[Rear ][rtp_timestamp]
Left  RTP -> jitter/reorder -> depayload/decode -> frame_by_ts[Left ][rtp_timestamp]
Right RTP -> jitter/reorder -> depayload/decode -> frame_by_ts[Right][rtp_timestamp]

when metadata and all four decoded frames exist for one rtp_timestamp:
    emit ServerFrameGroup(metadata, front, rear, left, right)
    erase all five cached entries
```

实现要求：

1. 在 depayload 前读取 RTP header，或通过所用媒体框架保留每个 Access Unit 的原始 RTP timestamp。
2. 对多包帧，以相同 RTP timestamp 聚合，Marker bit=1 表示帧边界。
3. 解码后必须把 RTP timestamp 作为 side data 与图像一起传递，不能在解码层丢失。
4. Metadata 通常在编码前提交，因此可能比视频帧先到；协议不保证到达顺序。
5. 所有缓存必须有容量和超时限制。建议以低延迟为优先，仅保留最近 16–64 组，
   或按现场网络把超时设为 100–500 ms。
6. 超时仍缺 metadata 或某路帧时，丢弃该不完整组并记录缺失原因，不允许无界等待。
7. 对 metadata 重复包和 RTP 重复包做幂等处理。
8. Jetson 重启或 RTP SSRC 变化时，清空对应流的旧缓存，重新建立时间戳上下文。

服务器组帧完成后，业务层使用 `group_id`/`trigger_cycle` 标识组，
RTP timestamp 主要用于传输层关联。

## 6. 服务器接收测试

### 6.1 Ubuntu 依赖

```bash
sudo apt update
sudo apt install -y \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-libav
```

正式服务器应根据 GPU 选择已验证的硬件解码器。
`avdec_h264` 只是通用正确性基线，不代表 4×1440×1080@80 FPS 的服务器性能方案。

### 6.2 单路 Front 显示

```bash
gst-launch-1.0 -v \
  udpsrc address=0.0.0.0 port=5000 \
    caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" \
  ! rtpjitterbuffer latency=50 drop-on-latency=true \
  ! rtph264depay ! h264parse ! avdec_h264 \
  ! videoconvert ! autovideosink sync=false
```

无图形界面时，将末尾替换为：

```text
! fpsdisplaysink video-sink=fakesink text-overlay=false sync=false
```

### 6.3 四路 2×2 预览

当前 Ubuntu/X11 接收机已验证：

```bash
cd /home/ubuntu/桌面/test/四路环视相机
./scripts/receive_four_grid.sh
```

布局为：

```text
Front | Rear
------+------
Left  | Right
```

该脚本只用于联调预览，输出窗口为 1280×960@25 FPS；
它不做 metadata 组帧，不是正式拼接服务器。

### 6.4 Metadata 可读性测试

```bash
python3 scripts/receive_metadata.py --bind 0.0.0.0 --port 5100
```

### 6.5 RTP/metadata 映射验收

先启动校验器，再启动 Jetson 发送端：

```bash
python3 scripts/verify_loopback_sync.py \
  --bind 0.0.0.0 \
  --active-streams 4 \
  --duration 16
```

该脚本直接绑定 5000/5002/5004/5006/5100，因此不能与正式接收程序或四宫格脚本同时运行。

成功输出示例：

```text
metadata packets=639 unique_ts=639 invalid=0
Front ... marker_frames=638 matched=638/638 (100.00%)
Rear  ... marker_frames=637 matched=637/637 (100.00%)
Left  ... marker_frames=631 matched=631/631 (100.00%)
Right ... marker_frames=628 matched=628/628 (100.00%)
RESULT=PASS
```

有限时长验收结束时，各路最后数帧数量可能略有不同；
判定依据是实际收到的帧与 metadata 映射比例，而不是截止瞬间的绝对数量。

## 7. Jetson 配置和启动

### 7.1 修改服务器 IP

Jetson 当前测试工程：

```text
/home/nvidia/four_camera_capture_test.4fo99V
```

编辑：

```bash
cd /home/nvidia/four_camera_capture_test.4fo99V
nano config/config.yaml
```

将 `server_ip` 改为正式服务器的 IPv4 地址：

```yaml
four_camera_capture:
  processing:
    output_fps: 80

  encoder:
    enabled: true
    codec: h264
    fps: 80
    bitrate_front: 20000000
    bitrate_rear: 20000000
    bitrate_left: 20000000
    bitrate_right: 20000000
    gop: 40
    low_latency: true
    active_cameras: 4
    queue_depth: 2

  output:
    mode: null
    queue_depth: 2

  network:
    server_ip: "<SERVER_IPV4>"
    payload_type: 96
    mtu: 1400
    metadata_enabled: true
    metadata_port: 5100
    metadata_queue_depth: 4

  streams:
    front_port: 5000
    rear_port: 5002
    left_port: 5004
    right_port: 5006
```

`--output rtp` 会覆盖配置中的 `output.mode`。
**程序只在启动时读取配置，修改 `server_ip` 后必须重启进程。**

### 7.2 环境检查

```bash
cd /home/nvidia/four_camera_capture_test.4fo99V
./scripts/check_jetson_multimedia.sh
```

结果必须为 `RESULT PASS`。必须使用 Jetson `nvv4l2h264enc`，不应静默切换为 x264 软编码。

### 7.3 构建与单元测试

正规构建命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DMVS_SDK_LIB_DIR=/opt/MVS/lib/aarch64
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

当前测试镜像也存在已构建目录 `build-phase4`。交付/部署应统一到 `build`，
不应长期依赖阶段性目录名。

### 7.4 启动四路 RTP 发送

先让服务器绑定所有接收端口，再在 Jetson 启动：

```bash
cd /home/nvidia/four_camera_capture_test.4fo99V
export LD_LIBRARY_PATH=/opt/MVS/lib/aarch64:/usr/local/cuda/lib64:${LD_LIBRARY_PATH:-}

./build-phase4/four_camera_capture \
  --config config/config.yaml \
  --encoder-cameras 4 \
  --output rtp
```

正式 `build` 目录完成后应使用：

```bash
./build/four_camera_capture \
  --config config/config.yaml \
  --encoder-cameras 4 \
  --output rtp
```

不提供 `--duration` 时会持续运行。按 `Ctrl+C` 发送 SIGINT 正常停止。

### 7.5 其他输出模式

纯性能测试，编码结果丢弃：

```bash
./build-phase4/four_camera_capture --config config/config.yaml \
  --encoder-cameras 4 --output null
```

保存四路裸 H.264：

```bash
./build-phase4/four_camera_capture --config config/config.yaml \
  --duration 10 --encoder-cameras 4 --output file
```

默认输出到 `encoded/front.h264`、`rear.h264`、`left.h264`、`right.h264`。

## 8. 启动和停止顺序

推荐启动顺序：

1. 确认服务器 IP、路由和防火墙。
2. 服务器绑定 metadata 和四路 RTP 端口。
3. 服务器启动解码、RTP timestamp side-data 和组帧缓存。
4. Jetson 启动四路采集/发送。
5. 检查 Jetson 每秒日志和服务器组帧成功率。

推荐停止顺序：

1. 向 Jetson 进程发送 SIGINT/SIGTERM，等待 `Four-camera capture stopped cleanly.`
2. 服务器等待短暂 drain timeout，丢弃仍不完整的组。
3. 停止服务器接收器。

不建议用 `kill -9`，因为这会跳过 Camera/GStreamer 正常释放。

## 9. 运行时健康指标

Jetson 日志每秒输出以下统计：

- Capture：每路约 98–100 FPS，`drop/timeout/grab_error/invalid` 不应持续增长。
- FrameGroup：`output_fps` 约 79–80，`sync_cycle_gap` 不应持续增长。
- ISP：`group_fps` 约 80，`isp_drop=0`，队列不持续满。
- Encode：每路约 79–80 FPS，`queue_drop=0 encoder_drop=0 errors=0`。
- Network：每路 `sent` 持续增长，`drop=0 errors=0`。
- Metadata：`sent` 约每秒增长 80，`drop=0 errors=0`。

当前 640×480 实测：

- 四路 ISP 平均约 0.8 ms/帧，P95 约 1.3 ms。
- 四路 H.264 编码平均约 8–9 ms，P95 约 13–15 ms。
- FrameGroup timestamp 到 RTP appsrc 成功接收平均约 17–18 ms，P95 约 28 ms。
- Jetson 到 Ubuntu 服务器局域网实测：约 639 metadata 组，四路已收帧与 metadata 映射率 100%。

服务器至少应监控：

- 每路 RTP packet/marker-frame 接收率、sequence gap、jitterbuffer 丢包和延迟。
- 每路解码 FPS/错误/耗时。
- Metadata 包率、无效包、重复包和 `group_id` 间隙。
- 完整四路组率、超时组率，以及每路缺帧数。
- 接收、解码和组帧缓存深度；不允许无界增长。
- CPU/GPU/RAM 和网卡 drop/error 计数。

## 10. 故障排查

### 服务器完全收不到 UDP

```bash
hostname -I
sudo tcpdump -ni any udp port 5000
```

检查：

- Jetson `config/config.yaml` 中 `server_ip` 是否为正确服务器 IP。
- 修改 IP 后是否已重启 Jetson 进程。
- Jetson 到服务器的路由：`ip route get <SERVER_IPV4>`。
- 服务器防火墙与 VLAN/Wi-Fi 客户端隔离。

### 有 UDP，但 GStreamer 不出图

检查：

- 是否显式设置了 H.264 RTP caps。
- `h264parse`、`rtph264depay` 和解码器插件是否安装。
- 服务器不是 Jetson 时，不要盲目使用 `nvv4l2decoder`；通用 Ubuntu 可先用 `avdec_h264` 验证。
- 等待下一次 SPS/PPS/IDR，通常不超过约 1 秒。

### 四宫格不出窗口或收到 EOS

- 先单独验证 5000/5002/5004/5006 都能解码。
- 使用仓库中已验证的 `scripts/receive_four_grid.sh`。
- 不要为当前脚本添加 `compositor ignore-inactive-pads=true`；在所有 live pad 首帧到达前，
  该选项可能导致立即 EOS。
- 确认 X11 环境下 `DISPLAY` 有效，并用 `videotestsrc ! videoconvert ! xvimagesink` 验证显示。

### 有视频，但无法组四路帧

- 确认 UDP 5100 有 136 字节 SVMD 数据报。
- 确认解码通路保留了原始 RTP timestamp，而不是仅保留服务器生成的显示 PTS。
- 确认按无符号 32 位值匹配，并正确处理时间戳回绕。
- 运行 `verify_loopback_sync.py`隔离验证协议层，但不要与正式接收程序同时绑定端口。

## 11. 实现边界与后续工作

当前 NX 路径不是 zero-copy：MVS buffer 复制到 host Bayer 池，上传 CUDA，
NV12 回传 host，再复制进 GStreamer/NVMM；编码 AU 也会经过 host 共享 buffer 和 RTP appsrc 复制。
所有队列有界且满时丢旧帧，设计优先级是 freshness 高于 completeness。

正式上线前尚需：

1. 在 4×1440×1080@80 FPS 配置下重新验证 USB、ISP、编码、网络和服务器解码性能。
2. 完成至少 30 分钟连续稳定性测试，检查内存增长、死锁、队列积压和网络丢包。
3. 确认正式服务器硬件解码能力和四路 80 FPS 组帧/算法通吐。
4. 如果跨不可信网络，在 RTP/UDP 之外设计认证、加密和可观测性机制。
5. 如果服务器要产生可与外部时钟对齐的时间，另行引入 PTP/NTP 时钟模型；
   不要把当前 steady-clock ns 当成 Unix epoch。

## 12. 关联文件

- `config/config.yaml`：Jetson 采集、ISP、编码、目标 IP 和端口配置。
- `src/network/streamer.cpp`：RTP payloader/UDP 实现。
- `src/network/metadata_sender.hpp/.cpp`：SVMD v1 常量、时间戳换算和序列化。
- `tests/metadata_wire_test.cpp`：metadata 协议单元测试。
- `scripts/receive_metadata.py`：参考解析器。
- `scripts/verify_loopback_sync.py`：RTP marker timestamp 与 metadata 映射验收器。
- `scripts/receive_four_grid.sh`：Ubuntu/X11 四路联调预览。
- `README.md`：项目总览、构建与当前实测结果。
