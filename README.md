# ai-media-player

音视频播放器，基于 FFmpeg + Qt + OpenGL 实现完整媒体处理链路

---

## 项目简介

本项目是一款面向"现代播放器工程能力"打磨的 C++17 音视频播放器，遵循标准媒体处理流程，已实现**本地/网络拉流、解封装、软/硬解码、音画同步、零拷贝渲染、录制**全链路能力。

项目采用**基于独立线程 + 有界阻塞队列的强类型固定拓扑节点图（DAG）** 架构：每个功能模块抽象为"独立线程 + 输入队列"的 Node，节点间通过强类型连接器 `Link<T>` 串联，控制信令通过共享的 `SyncContext` 总线以**无锁 atomic epoch** 广播。相较早期的"上帝对象 + Listener 回调链"，新架构消除了控制流反转、跨线程裸指针生命周期风险与 Seek 竞态。

本期（30 天）聚焦四大技术攻坚方向：

- **DAG 流水线架构**：`std::condition_variable` 有界阻塞队列背压防 OOM；`atomic epoch` 驱动 Seek 等全局信令，彻底废弃旧的多标志握手式 Seek。
- **VA-API 硬件解码 + 零拷贝渲染**：`vaExportSurfaceHandle` 导出 DMA-BUF fd，`eglCreateImageKHR` 包装为 OpenGL 纹理，消除显存→内存→显存的 CPU 帧拷贝。
- **GLSL 高级色彩管线**：BT.601/709/2020 矩阵动态切换，PQ EOTF 反变换 + Reinhard 完成 HDR→SDR Tone Mapping。
- **全链路 QoS**：HTTP/RTP 拉流 + 抗弱网 Jitter Buffer（序列号乱序/空洞检测、自适应缓存深度、GOP Cache 首帧秒开 <180ms）；非对称音画同步（±40ms）与卡顿率监控。

> 决策说明：本期**废弃自写 FLV/MP4 解封装**计划，容器解析统一由 `FfmpegDemuxerNode` 兜底，将预算集中投入硬解与 QoS；WebRTC **不集成源码**，其核心机制（Jitter Buffer / FEC）选择性落地至网络层，完整协议栈以技术白皮书形式产出。

---

## 架构设计

项目采用 **强类型固定拓扑的节点图（DAG）** 架构。每个功能模块为独立线程 + 输入队列的 Node，节点之间通过强类型连接器（`Link<T>`）串联，控制信令通过共享 `SyncContext` 总线以 atomic epoch 广播，面向"网络拉流、硬件加速零拷贝、色彩空间/HDR、QoS"等方向预留扩展位。

### 1. 源码目录结构

采用 Google C++ Style 分层：对外公共 API 收敛于 `include/mediacore/`（以静态库名 `mediacore` 做前缀目录，`#include "mediacore/xxx.h"` 防冲突、来源自明），实现细节隐藏于 `src/`。

```
ai-media-player/
├── CMakeLists.txt
├── build.sh
├── conanfile.py
├── mediacore/                                 # 核心媒体引擎静态库（libmediacore）
│   ├── include/mediacore/                     # 对外暴露的公共 API（PUBLIC 头，以库名做前缀目录）
│   │   ├── iplayer.h                          # Facade 接口（UI 唯一依赖）
│   │   ├── ivideo_sink.h                      # UI 渲染注入点
│   │   ├── igl_context.h                      # 共享 GL 上下文接口
│   │   ├── playback_state.h
│   │   ├── logger.h
│   │   └── av_types/                          # 公共数据类型（DTO）
│   │       ├── byte_chunk.h                   # 原始字节流（Source 输出）
│   │       ├── packet.h                       # 压缩包（codec_id/pts_us/dts_us/flags/epoch）
│   │       ├── video_frame.h                  # sum type：Software / Hardware variant
│   │       ├── audio_frame.h                  # PCM 帧
│   │       └── stream_info.h                  # VideoStreamInfo / AudioStreamInfo
│   │
│   └── src/                                   # 内部实现（PRIVATE）
│       ├── core/                              # DAG 框架
│       │   ├── inode.h                        # 节点基类模板 INode<InT, OutT> + INodeBase
│       │   ├── link.h                         # 强类型连接器（有界阻塞队列封装）
│       │   ├── pipeline.h/.cpp                # 容器：Build / Start / Stop / Seek
│       │   ├── fsm.h/.cpp                      # 显式状态机（合法跃迁表）
│       │   ├── sync_context.h                 # 控制总线（atomic epoch + state）
│       │   ├── iclock_source.h                # 时钟接口 IClockSource
│       │   └── player_facade.h/.cpp           # 对 UI 的门面（实现 IPlayer）
│       │
│       ├── source/                            # 数据源层
│       │   ├── isource_node.h
│       │   ├── file_source_node.h/.cpp        # 本地分块读 + 字节 Seek
│       │   ├── http_source_node.h/.cpp        # libcurl 流式 GET（Range）
│       │   └── rtp_source_node.h/.cpp         # UDP recv + RTP 头解析
│       │
│       ├── network/                           # QoS 抗弱网层（仅网络源接入）
│       │   ├── jitter_buffer_node.h/.cpp      # seq 乱序/空洞检测 + 自适应深度
│       │   ├── gop_cache.h/.cpp               # 最近 GOP 缓存，支撑首帧秒开
│       │   ├── fec_decoder_node.h/.cpp        # XOR FEC 丢包恢复
│       │   └── rtp_depacketizer_node.h/.cpp   # FU-A / STAP-A 组帧
│       │
│       ├── demux/
│       │   └── ffmpeg_demuxer_node.h/.cpp     # libavformat 统一兜底（双输出）
│       │
│       ├── decode/
│       │   ├── video_decoder_node.h/.cpp      # 软解 / VA-API 硬解双模式
│       │   ├── audio_decoder_node.h/.cpp      # avcodec + swr 重采样
│       │   └── hw_device_context.h/.cpp       # av_hwdevice_ctx 封装与探测
│       │
│       ├── filter/
│       │   └── tee_node.h                     # 帧多播（录制分支，shared_ptr 零拷贝转发）
│       │
│       ├── sync/
│       │   └── av_sync_node.h/.cpp            # 非对称音画同步（±40ms）
│       │
│       ├── encode/
│       │   ├── video_encoder_node.h/.cpp      # CBR/CRF/CQP 码率控制
│       │   ├── audio_encoder_node.h/.cpp      # AAC FLTP 编码
│       │   └── rate_control_config.h
│       │
│       ├── mux/
│       │   └── mp4_muxer_node.h/.cpp          # av_interleaved_write_frame 交错写盘
│       │
│       ├── sink/                              # 终端输出层（DAG 汇聚点，与 source/ 对称）
│       │   ├── video_sink_node.h/.cpp         # 把帧推给外部 IVideoSink（OpenGLView）
│       │   ├── audio_sink_node.h/.cpp         # 消费 PCM，持 IAudioSink 后端，兼 IClockSource（音频主时钟）
│       │   ├── iaudio_sink.h                  # 音频输出后端抽象（可切 Alsa / Qt / PipeWire）
│       │   └── alsa_audio_sink.h/.cpp         # IAudioSink 的 ALSA 实现（libasound / snd_pcm_writei）
│       │
│       ├── qos/
│       │   └── qos_monitor.h/.cpp             # 卡顿率/丢帧率/缓存深度/端到端延迟
│       │
│       └── utils/
│           ├── thread_safe_queue.h            # 泛型有界阻塞队列（背压）
│           └── logger.cpp
│
└── app/                                       # 桌面应用层（Qt GUI 前端）
    ├── main.cpp
    ├── main_window.h/.cpp
    └── ui/
        ├── player_widget.h/.cpp               # 持 IPlayer（Facade）
        ├── opengl_view.h/.cpp                 # 双路径渲染分派（sw / hw）
        ├── controller_widget.h/.cpp           # 播放控制 + QoS 指标显示
        └── render/
            ├── sw_texture_uploader.h/.cpp     # 软件 YUV420P 三平面上传
            ├── hw_texture_importer.h/.cpp     # 硬件零拷贝（DMA-BUF / EGLImage）
            └── color_shader.h                 # BT.601/709/2020 + HDR Tone Mapping
```

### 2. 类继承与组合树

```
IPlayer (Facade 接口)
└── PlayerFacade
    ├── FSM (显式状态机，合法跃迁表)
    ├── SyncContext (控制总线：atomic epoch + atomic state)
    └── Pipeline
        └── std::vector<unique_ptr<INodeBase>>
            ├── ISourceNode<void, ByteChunk>
            │   ├── FileSourceNode
            │   ├── HttpSourceNode
            │   └── RtpSourceNode
            ├── RtpDepacketizerNode<RtpPacket, RtpPacket>    (仅网络源)
            ├── FecDecoderNode<RtpPacket, RtpPacket>          (仅网络源)
            ├── JitterBufferNode<RtpPacket, ByteChunk>        (仅网络源，持 GopCache)
            ├── FfmpegDemuxerNode<ByteChunk, Packet>          (多输出: video / audio)
            ├── INode<Packet, VideoFrame>
            │   └── VideoDecoderNode (sw / VA-API，持 HwDeviceContext)
            ├── INode<Packet, AudioFrame>
            │   └── AudioDecoderNode
            ├── TeeNode<T>                                    (多输出：主路径 / 录制)
            ├── AVSyncNode (依赖 IClockSource)
            ├── INode<Frame, Packet>
            │   ├── VideoEncoderNode (CBR/CRF/CQP)
            │   └── AudioEncoderNode
            ├── Mp4MuxerNode<Packet, void>                    (多输入)
            └── ISinkNode<Frame, void>
                ├── VideoSinkNode (→ IVideoSink / OpenGLView)
                └── AudioSinkNode (持 IAudioSink = AlsaAudioSink，兼 IClockSource)
```

### 3. 主播放管线（含录制分支）

```
        ┌──────────────┐
        │ Http / Rtp   │  线程: SourceThread
        │  SourceNode  │  输出: shared_ptr<ByteChunk> / RtpPacket
        └──────┬───────┘  动作: 网络读字节 / UDP recv
               │
               ▼
        ┌──────────────┐
        │ JitterBuffer │  线程: 自有   （仅网络源接入）
        │  + FEC + GOP │  动作: seq 重排、空洞检测、自适应深度、秒开
        └──────┬───────┘
               │ byte stream
               ▼
      ┌──────────────────┐
      │ FfmpegDemuxerNode│  线程: DemuxerThread
      │   (libavformat)  │  输入: ByteChunk（AVIOContext 自定义读回调）
      └────┬─────────┬───┘  输出: Packet（video / audio 两路，打 epoch 戳）
           │ video   │ audio
   Packet(H264/HEVC)  Packet(AAC)
           ▼          ▼
   ┌──────────────┐ ┌──────────────┐
   │VideoDecoder  │ │AudioDecoder  │  线程: 各自独立
   │(VA-API / sw) │ │(+ swr)       │
   └──────┬───────┘ └──────┬───────┘
 VideoFrame │              │  AudioFrame (PCM S16)
 (hw/sw     │              │
  variant)  ▼              ▼
   ┌──────────────┐ ┌──────────────┐
   │  VideoTee    │ │  AudioTee    │  线程: 上游驱动
   │ (录制多播)   │ │              │  shared_ptr 零拷贝转发
   └──┬───────┬───┘ └──┬───────┬───┘
      │主路径 │录制     │主路径 │录制
      ▼       ▼        ▼       ▼
 ┌──────────┐┌──────────────┐┌──────────┐┌──────────────┐
 │ AVSync   ││VideoEncoder  ││AudioSink ││AudioEncoder  │
 │(查音频   ││(CBR/CRF/CQP) ││(ALSA主钟)││(AAC)         │
 │ 主时钟)  ││              ││          ││              │
 └────┬─────┘└──────┬───────┘└──────────┘└──────┬───────┘
      │ VideoFrame  │ Packet(H264)               │ Packet(AAC)
      │ (synced)    └──────────┬─────────────────┘
      ▼                        ▼
 ┌──────────────┐        ┌──────────────┐
 │ VideoSink    │        │  Mp4Muxer    │  线程: MuxerThread
 └──────┬───────┘        └──────────────┘  动作: 交错写盘
        │ VideoFrame (sw / hw variant)
        ▼
 ┌──────────────────────────────┐
 │ OpenGLView                   │  线程: Qt 主线程（paintGL）
 │  ├─ sw:  三平面 glTexSubImage │  Software variant → CPU 上传
 │  ├─ hw:  EGLImage 零拷贝导入   │   Hardware variant → DMA-BUF 直接成纹理
 │  └─ ColorShader: BT.x + HDR  │  矩阵动态切换 + Tone Mapping
 └──────────────────────────────┘
```

### 4. 每个节点的详细规格

| 节点 | 输入类型 | 输出类型 | 主要动作 | 线程 |
|------|---------|---------|---------|-----|
| **FileSourceNode** | – | `ByteChunk` | 分块读文件 + 字节偏移 Seek | 自有 |
| **HttpSourceNode** | – | `ByteChunk` | libcurl 流式 GET，支持 Range | 自有 |
| **RtpSourceNode** | – | `RtpPacket` | UDP recv，解析 RTP 头（seq/ts/marker） | 自有 |
| **RtpDepacketizerNode** | `RtpPacket` | `RtpPacket` | H.264 FU-A / STAP-A 组帧 | 自有 |
| **FecDecoderNode** | `RtpPacket` | `RtpPacket` | XOR 异或恢复丢包 | 自有 |
| **JitterBufferNode** | `RtpPacket` | `ByteChunk` | seq 排序、空洞检测、自适应深度、GOP 秒开 | 自有 |
| **FfmpegDemuxerNode** | `ByteChunk`（或 url） | `Packet`×2 | libavformat 分包，打 epoch 戳 | 自有 |
| **VideoDecoderNode** | `Packet` | `VideoFrame` | sw: avcodec / hw: VA-API 显存帧 | 自有 |
| **AudioDecoderNode** | `Packet` | `AudioFrame` | avcodec + swr 重采样 | 自有 |
| **TeeNode\<T\>** | `T` | `T`×N | `shared_ptr` 复制多播（可开关） | 上游驱动 |
| **AVSyncNode** | `VideoFrame` | `VideoFrame` | 查音频主时钟，±40ms 非对称丢/等/送 | 自有 |
| **VideoEncoderNode** | `VideoFrame` | `Packet` | x264 + CBR/CRF/CQP 码率控制 | 自有 |
| **AudioEncoderNode** | `AudioFrame` | `Packet` | AAC FLTP 编码 | 自有 |
| **Mp4MuxerNode** | `Packet`×2 | – | `av_interleaved_write_frame` | 自有 |
| **VideoSinkNode** | `VideoFrame` | – | 调 `IVideoSink::OnReceiveVideoFrame` | 上游驱动 |
| **AudioSinkNode** | `AudioFrame` | – | 经 `IAudioSink`(AlsaAudioSink) `snd_pcm_writei` 投递 PCM，维护音频主时钟 | 自有 |

### 5. 数据类型流转链

```
原始字节          ByteChunk { uint8_t* data, size_t size, int64_t recv_time_us, uint64_t epoch }
   │
   ▼ (网络源) RTP 组帧 / FEC 恢复 / JitterBuffer 重排 / GOP 秒开
   │
   ▼ Demux 拆容器、分流
压缩包            Packet { AVPacket*, codec_id, pts_us, dts_us, frame_kind, flags, stream, epoch }
   │
   ▼ Decode 解压（软解 or VA-API 硬解）
未压缩帧          VideoFrame { color_space, color_trc, color_range, flags, epoch, ... }
                              ├─ Software variant: data[3] + linesize[3] + buffer_（紧凑 YUV）
                              └─ Hardware variant: AVBufferRef* + DMA-BUF fd + hw_type
                  AudioFrame { AVFrame*, pts_us, sample_rate, channels }
   │
   ├──→ 编码分支：重新变回 Packet（H264 / AAC）→ Mux 写盘
   │
   └──→ 渲染分支：
        ├─ Software variant → glTexSubImage2D 三平面上传 → GL Texture
        └─ Hardware variant → EGLImage 零拷贝导入 → GL Texture
        Audio → PCM bytes → AudioSinkNode → ALSA (snd_pcm_writei) → 声卡
```

要点：

- **`ByteChunk`**：原始字节流，仅在 Source→Demuxer（网络源额外经 JitterBuffer）之间存在；解耦"如何拿到字节"与"如何解析容器"。
- **`Packet`**：保留 `AVPacket*` 内核（供下游 libavcodec），携带业务级元信息（`codec_id`/`pts_us`/`dts_us`/`frame_kind`/`flags`/`stream`）以及 **`epoch`**（Seek 世代号）。
- **`VideoFrame`** 为 sum type：`Software` 携带紧凑 YUV buffer；`Hardware` 携带 `AVBufferRef*` + DMA-BUF fd，使零拷贝与软件路径共用同一条管线；额外携带 `color_space/color_trc/color_range` 供 GLSL 色彩管线动态选择矩阵与 Tone Mapping。
- **`AudioFrame`** 保持轻包装，重采样/通道布局在 `AudioDecoderNode` 内消化。

### 6. 控制信令流

控制信令独立于数据通道传播，核心是**无锁 atomic epoch（世代号）机制**，替代旧架构跨线程的多标志握手，从根本上消除 Seek 竞态。

```
UI (Qt 主线程)
  │ Play / Pause / Stop / Seek
  ▼
PlayerFacade → FSM.Dispatch(event)  （校验合法跃迁）
  │
  ├─ 普通状态变更：修改 SyncContext.state_，notify_all(state_cv_)
  │     所有节点 ThreadLoop 被唤醒，各自读 state 决定 继续/挂起/退出
  │
  └─ Seek：SyncContext.epoch_.fetch_add(1)   （世代号 +1，无锁）
        │
        ▼
     Source/Demuxer 物理重定位到目标位置，此后产出的 Packet/Frame 均打上"新 epoch"
        │
        ▼
     各节点在处理每个数据项时比较 item.epoch 与全局 epoch：
        item.epoch < 当前 epoch  →  陈旧数据（seek 前遗留），无条件丢弃
        item.epoch == 当前 epoch →  正常处理
```

- **atomic epoch（带外世代号）**：Seek 只做一件事——原子自增全局 epoch。所有 seek 前产生的、仍在各队列或"在飞"中的数据，其 epoch 已过期，被各节点自然丢弃，**无需 Player 反向下钻清空每个队列，无需 5 个原子标志跨线程握手**。这解决了旧架构"已 Pop 的在飞帧无法冲刷 → 花屏/错位"的结构性问题。
- **带内信令（Flush / EOF）** 仍沿数据通道顺序传播，保证与数据流时序一致：
  - **Flush**：Seek 后由 Demuxer 在新位置数据之前注入一个 `kFlush` 空 Packet，解码器遇到时执行 `avcodec_flush_buffers` 重置参考帧，避免解码花屏。
  - **EOF**：Demuxer 读到 EOF 注入 `kEOF`，末端节点（AVSync / Speaker）收到后经 Facade 判定全局播放完成。

---

## 技术栈

| 技术领域 | 选型方案 | 对标亮点 |
|----------|----------|---------|
| 核心开发语言 | C/C++ 17（Google C++ Style Guide） | – |
| 媒体处理框架 | FFmpeg（libavcodec / libavformat / libavutil / libswscale / libswresample） | DAG / 硬解 |
| 硬件解码 | VA-API（`av_hwdevice_ctx` + `AV_PIX_FMT_VAAPI` + `get_format` 回调） | 硬解零拷贝 |
| 零拷贝互操作 | `vaExportSurfaceHandle` → DMA-BUF fd → `eglCreateImageKHR` → `glEGLImageTargetTexture2DOES` | 硬解零拷贝 |
| 视频渲染引擎 | OpenGL 3.3 Core / EGL | 零拷贝 / GLSL |
| 色彩管线 | GLSL：BT.601/709/2020 矩阵动态切换 + PQ EOTF 反变换 + Reinhard Tone Mapping | GLSL 色彩 |
| 音频后端 | ALSA（libasound）原生 PCM 投递；`IAudioSink` 抽象（可切 QtAudioSink / PipeWire）；兼音频主时钟 IClockSource | 音画同步 |
| 并发模型 | 独立线程 + `std::condition_variable` 有界阻塞队列（背压防 OOM）+ atomic epoch | DAG |
| 网络拉流 | libcurl（HTTP/Range）、原生 UDP + RTP | QoS |
| 抗弱网 QoS | Jitter Buffer（seq 重排/空洞检测/自适应深度）+ GOP Cache（秒开 <180ms）+ XOR FEC | QoS |
| 界面框架 | Qt 6.x | – |
| 日志 | spdlog | – |
| 构建工具 | CMake 3.23+ | – |
| 依赖管理 | Conan 2 | – |
| 主开发环境 | Ubuntu 24.04 | – |
| 版本控制 | GitLab | – |

---

## 环境搭建

### 依赖安装（Ubuntu 24.04）

```bash
# 更新软件源
sudo apt update && sudo apt upgrade -y

# Qt 6 开发套件
sudo apt install -y qt6-base-dev qt6-multimedia-dev qt6-tools-dev

# OpenGL / EGL 开发环境（含硬件解码零拷贝所需的 EGL 头）
sudo apt install -y libgl1-mesa-dev libegl1-mesa-dev libglu1-mesa-dev

# VA-API 硬件解码运行时与开发头 + DMA-BUF 支持
sudo apt install -y vainfo libva-dev libva-drm2 libdrm-dev

# 网络拉流依赖（HTTP）
sudo apt install -y libcurl4-openssl-dev

# 音频输出（ALSA 原生 PCM 投递）
sudo apt install -y libasound2-dev

# 构建工具与编译器
sudo apt install -y cmake build-essential gdb git pkg-config

# 验证 VA-API 硬件解码能力（应能列出 H264/HEVC 的 VLD 解码入口）
vainfo
```

> FFmpeg / spdlog / stb / glm 由 Conan2 管理；ALSA 走系统包 `libasound2-dev`（非 Conan）。

---

## 编译与运行

```bash
# 1. 克隆项目
git clone https://git.cpptrain.top/zhanmeng/ai-media-player.git
cd ai-media-player

# 2. 一键构建（默认 RelWithDebInfo，可传 Release / Debug）
./build.sh

# 3. 运行
./build/RelWithDebInfo/ai_media_player
```

---

## 文档导航

| 文档 | 说明 |
|-----|------|
| [README.md](./README.md) | 项目总览与架构设计（本文件） |
| [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md) | 30 天重构 + 演进路线的详细开发计划书 |

---

## 开发规范

### 一、分支管理规范

| 分支类型 | 命名规范 | 用途说明 |
|----------|----------|----------|
| 主分支 | `main` | 仅存放稳定、可运行的代码，禁止直接提交，仅接受 MR 合并 |
| 重构分支 | `refactor/<scope>` | 架构性改造，如 `refactor/dag` |
| 功能分支 | `feat/<模块名-功能描述>` | 新功能、新模块开发，如 `feat/vaapi-zerocopy` |
| 修复分支 | `fix/<问题描述>` | bug 修复，如 `fix/av-sync-drift` |
| 文档分支 | `docs/<文档内容>` | 文档更新 |

### 二、Commit 提交规范

#### 1. Commit 消息格式

```
<type>(<scope>): <subject>

<body>

<footer>
```

#### 2. type 可选值

| 类型 | 说明 |
|------|------|
| `feat` | 新增功能 |
| `fix` | 修复 Bug |
| `docs` | 文档更新 |
| `style` | 代码格式调整，不影响代码逻辑 |
| `refactor` | 代码重构 |
| `test` | 测试用例 |
| `chore` | 构建过程、依赖库变动 |

#### 3. Commit 示例

```
feat(vaapi-zerocopy): 打通 VA-API 硬解 DMA-BUF 零拷贝渲染

- av_hwdevice_ctx 创建 VAAPI 上下文，get_format 回调返回 AV_PIX_FMT_VAAPI
- vaExportSurfaceHandle 导出 DMA-BUF fd，eglCreateImageKHR 包装为 GL 纹理
- 4K@60 HEVC 实测 CPU 占用由 85% 降至 18%，画面无退化

Closes #23
```
