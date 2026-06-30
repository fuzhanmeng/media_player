# ai-media-player
音视频播放器，基于 FFmpeg + Qt + OpenGL 实现完整媒体处理链路

---

## 项目简介
本项目是一款功能完整的 C++ 音视频播放器，遵循标准媒体处理流程，已实现**文件读取、解封装、解码、音画同步、基础音视频效果处理、渲染播放**全链路能力
项目采用**基于独立线程 + 阻塞队列的节点图（DAG）** 架构设计，各模块以 Node 形式独立运行、强类型连接，预留 AI 音视频增强、硬件加速、自写流媒体协议、QoS 等扩展接口

---

## 架构设计

项目采用 **强类型固定拓扑的节点图（DAG）** 架构。每个功能模块抽象为独立线程 + 输入队列的 Node，节点之间通过强类型连接器（Link）串联，控制信令通过共享的 SyncContext 总线广播。该架构面向"自写流媒体协议 / 容器解析、硬件加速零拷贝、码率控制、色彩空间转换、QoS"等扩展方向预留位置

### 1. 源码目录结构

```
ai-media-player/
├── CMakeLists.txt
├── learn_av/                              # 核心引擎静态库
│   ├── include/                           # 对外暴露的公共 API
│   │   ├── iplayer.h                      # Facade 接口
│   │   ├── ivideo_sink.h                  # UI 注入点
│   │   ├── playback_state.h
│   │   ├── logger.h
│   │   └── av_types/                      # 公共数据类型
│   │       ├── byte_chunk.h               # 原始字节流（Source 输出）
│   │       ├── packet.h                   # 压缩包 (含 codec_id/pts_us/frame_kind)
│   │       ├── video_frame.h              # sum type: Software/Hardware variant
│   │       ├── audio_frame.h              # PCM 帧
│   │       └── stream_info.h              # VideoStreamInfo / AudioStreamInfo
│   │
│   └── src/
│       ├── core/                          # DAG 框架
│       │   ├── inode.h                    # 节点基类模板 INode<InT, OutT>
│       │   ├── link.h                     # 强类型连接器（队列封装）
│       │   ├── pipeline.h/.cpp            # 容器：Build/Start/Stop
│       │   ├── fsm.h/.cpp                 # 显式状态机
│       │   ├── sync_context.h             # 控制总线
│       │   ├── clock.h                    # 时钟接口 IClockSource
│       │   └── player_facade.h/.cpp       # 对 UI 的门面
│       │
│       ├── source/                        # 数据源层
│       │   ├── isource_node.h
│       │   ├── file_source_node.h/.cpp
│       │   ├── http_source_node.h/.cpp
│       │   ├── hls_source_node.h/.cpp
│       │   └── rtp_source_node.h/.cpp
│       │
│       ├── demux/                         # 解封装节点群
│       │   ├── ffmpeg_demuxer_node.h/.cpp # 用 libavformat（兜底）
│       │   ├── flv_demuxer_node.h/.cpp    # 自写 FLV 解析
│       │   └── mp4_demuxer_node.h/.cpp    # 自写 MP4 解析
│       │
│       ├── decode/
│       │   ├── video_decoder_node.h/.cpp  # 支持 sw / hw 双模式
│       │   ├── audio_decoder_node.h/.cpp
│       │   └── hw_device_ctx.h/.cpp       # 硬件上下文
│       │
│       ├── network/                       # QoS 层
│       │   ├── jitter_buffer_node.h/.cpp
│       │   ├── fec_decoder_node.h/.cpp
│       │   └── rtp_depacketizer_node.h/.cpp
│       │
│       ├── filter/                        # 滤镜与分支
│       │   ├── tee_node.h                 # 帧多播（录制分支）
│       │   └── color_convert_node.h/.cpp  # 软件色彩转换（编码前用）
│       │
│       ├── sync/
│       │   └── av_sync_node.h/.cpp
│       │
│       ├── encode/
│       │   ├── video_encoder_node.h/.cpp  # 支持 CBR/CRF/CQP
│       │   ├── audio_encoder_node.h/.cpp
│       │   └── rate_control_config.h
│       │
│       ├── mux/
│       │   └── mp4_muxer_node.h/.cpp
│       │
│       ├── output/
│       │   ├── sdl_audio_speaker_node.h/.cpp
│       │   └── video_sink_node.h/.cpp     # 把帧推给外部 IVideoSink
│       │
│       └── utils/
│           ├── thread_safe_queue.h
│           └── logger.cpp
│
└── qt/                                    # UI 层
    ├── main.cpp
    ├── main_window.h/.cpp
    └── ui/
        ├── player_widget.h/.cpp           # 持 IPlayer（Facade）
        ├── opengl_view.h/.cpp             # 双路径渲染 + Shader 升级
        ├── controller_widget.h/.cpp
        └── render/
            ├── sw_texture_uploader.h/.cpp # 软件 YUV 上传
            ├── hw_texture_importer.h/.cpp # 硬件零拷贝（EGLImage/IOSurface）
            └── color_shader.h             # BT.601/709/2020 + HDR Tone Mapping
```
### 2. 类继承与组合树

```
IPlayer (Facade)
└── PlayerFacade
    ├── FSM (状态机)
    ├── SyncContext (控制总线)
    └── Pipeline
        └── std::vector<unique_ptr<INodeBase>>
            ├── ISourceNode<void, ByteChunk>
            │   ├── FileSourceNode
            │   ├── HttpSourceNode
            │   └── RtpSourceNode
            ├── IDemuxerNode<ByteChunk, Packet>          (多输出: video/audio)
            │   ├── FfmpegDemuxerNode
            │   ├── FlvDemuxerNode
            │   └── Mp4DemuxerNode
            ├── INode<Packet, Frame>
            │   ├── VideoDecoderNode (sw/hw)
            │   └── AudioDecoderNode
            ├── TeeNode<T>                                (多输出)
            ├── AVSyncNode (依赖 IClockSource)
            ├── INode<Frame, Packet>
            │   ├── VideoEncoderNode
            │   └── AudioEncoderNode
            ├── Mp4MuxerNode<Packet, void>               (多输入)
            └── ISinkNode<Frame, void>
                ├── VideoSinkNode (→ OpenGLView)
                └── SdlAudioSpeakerNode (兼 IClockSource)
```

### 3. 主播放管线（含录制分支）

```
                     ┌─────────────┐
                     │ FileSource  │  线程: SourceThread
                     │  (or Http/  │  输出: shared_ptr<ByteChunk>
                     │   Rtp)      │  动作: 文件/网络读字节
                     └──────┬──────┘
                            │ byte stream
                            ▼
                     ┌─────────────┐
                     │JitterBuffer │  仅网络源接入（阶段 8）
                     │ + FEC       │  按 RTP seq 排序、丢包恢复
                     └──────┬──────┘
                            │
                            ▼
                  ┌──────────────────┐
                  │ Demuxer 节点     │  线程: DemuxerThread
                  │ (ffmpeg/flv/mp4) │  输入: ByteChunk
                  │                  │  输出: Packet (video / audio 两路)
                  └────┬─────────┬───┘
                       │ video   │ audio
              Packet(H264/HEVC)  Packet(AAC)
                       ▼         ▼
              ┌──────────────┐ ┌──────────────┐
              │VideoDecoder  │ │AudioDecoder  │  线程: 各自独立
              │(sw or hw)    │ │              │
              └──────┬───────┘ └──────┬───────┘
        VideoFrame   │                │   AudioFrame (PCM S16)
        (sw or hw    │                │
         variant)    ▼                ▼
              ┌──────────────┐ ┌──────────────┐
              │  VideoTee    │ │  AudioTee    │  线程: 上游线程驱动
              │ (多播 + 录制 │ │              │  shared_ptr 引用计数
              │  开关)       │ │              │  零拷贝转发
              └──┬───────┬───┘ └──┬───────┬───┘
                 │主路径 │录制    │主路径 │录制
                 ▼       ▼       ▼       ▼
          ┌──────────┐ ┌──────────────┐  ┌──────────┐ ┌──────────────┐
          │ AVSync   │ │VideoEncoder  │  │SdlSpeaker│ │AudioEncoder  │
          │(查audio_ │ │(CBR/CRF/CQP) │  │(主时钟)  │ │(AAC)         │
          │ clock)   │ │              │  │          │ │              │
          └────┬─────┘ └──────┬───────┘  └──────────┘ └──────┬───────┘
               │ VideoFrame   │ Packet(H264)                  │ Packet(AAC)
               │ (synced)     └──────────┬───────────────────┘
               ▼                         ▼
        ┌──────────────┐          ┌──────────────┐
        │ VideoSink    │          │  Mp4Muxer    │  线程: MuxerThread
        │ (sw upload / │          │              │  动作: 交错写盘
        │  hw EGLImage)│          └──────────────┘
        └──────┬───────┘
               │ OpenGL textures
               ▼
        ┌──────────────┐
        │ OpenGLView   │  线程: Qt 主线程（paintGL）
        │ + ColorShader│  动作: BT.x 矩阵 + Tone Mapping
        │ + Tone Map   │
        └──────────────┘
```

### 4. 每个节点的详细规格

| 节点 | 输入类型 | 输出类型 | 主要动作 | 线程 |
|------|---------|---------|---------|-----|
| **FileSourceNode** | – | `ByteChunk` | `fread`/mmap 分块读 | 自有 |
| **HttpSourceNode** | – | `ByteChunk` | libcurl 流式 GET | 自有 |
| **RtpSourceNode** | – | `RtpPacket` | UDP recv | 自有 |
| **JitterBufferNode** | `RtpPacket` | `RtpPacket` | seq 排序、抖动平滑 | 自有 |
| **FecDecoderNode** | `RtpPacket` | `RtpPacket` | XOR 恢复丢包 | 自有 |
| **FfmpegDemuxerNode** | `ByteChunk`（或自带 url） | `Packet`×2 | libavformat 分包 | 自有 |
| **FlvDemuxerNode** | `ByteChunk` | `Packet`×2 | 手撕 FLV Tag 字节 | 自有 |
| **Mp4DemuxerNode** | `ByteChunk` | `Packet`×2 | moov/mdat 原子解析 | 自有 |
| **VideoDecoderNode** | `Packet` | `VideoFrame` | sw: avcodec / hw: 显存帧 | 自有 |
| **AudioDecoderNode** | `Packet` | `AudioFrame` | avcodec + swr 重采样 | 自有 |
| **TeeNode<T>** | `T` | `T`×N | `shared_ptr` 复制多播 | 上游驱动 |
| **AVSyncNode** | `VideoFrame` | `VideoFrame` | 查时钟、丢/睡/送 | 自有 |
| **VideoEncoderNode** | `VideoFrame` | `Packet` | x264 + 码率控制 | 自有 |
| **AudioEncoderNode** | `AudioFrame` | `Packet` | AAC FLTP 编码 | 自有 |
| **Mp4MuxerNode** | `Packet`×2 | – | `av_interleaved_write_frame` | 自有 |
| **VideoSinkNode** | `VideoFrame` | – | 调 `IVideoSink::OnReceive` | 上游驱动 |
| **SdlAudioSpeakerNode** | `AudioFrame` | – | SDL 回调拉数据 | SDL 回调线程 |

### 5. 数据类型流转链

```
原始字节            ByteChunk { uint8_t* data, size_t size, int64_t recv_time_us }
   │
   ▼ Demux 拆容器、分流
压缩包              Packet { AVPacket*, codec_id, pts_us, dts_us, frame_kind, flags, stream }
   │
   ▼ Decode 解压
未压缩帧            VideoFrame { storage, pix_fmt, color_space, color_trc, ... }
                                 ├─ Software variant: data[3], linesize[3], buffer_
                                 └─ Hardware variant: hw_frames_ctx, hw_handle, hw_type
                    AudioFrame { AVFrame*, pts_us, sample_rate, channels }
   │
   ├──→ 编码分支：重新变回 Packet（含 H264/AAC）
   │
   └──→ 渲染分支：VideoFrame → GL Texture / Audio → PCM bytes → 硬件
```

要点：

- **`ByteChunk`**：原始字节流，仅在 Source → Demuxer 之间存在；承担"如何拿到字节"与"如何解析容器"的解耦
- **`Packet`**：保留 `AVPacket*` 内核（便于下游 libavcodec），同时携带业务级元信息（`codec_id` / `pts_us` / `dts_us` / `frame_kind` / `flags` / `stream`），让自写 demuxer 与 libavformat 输出格式一致
- **`VideoFrame`** 设计为 sum type：`Software` 携带紧凑 YUV buffer，`Hardware` 携带 `AVBufferRef*` + 平台句柄（DMA-BUF / IOSurface / D3D Surface），让零拷贝路径与软件路径共用同一条管线
- **`AudioFrame`** 保持轻包装，所有重采样/通道布局在 AudioDecoderNode 内消化

### 6. 控制信令流

控制信令独立于数据通道传播，避免与数据帧时序竞争。

```
UI (Qt 主线程)
  │ Play / Pause / Stop / Seek
  ▼
PlayerFacade
  │
  ▼
FSM.Dispatch(event)
  │ 修改 SyncContext.state_
  │ notify_all(state_cv_)
  ▼
所有节点的 ThreadLoop 同时被唤醒
  └─ 各自读 SyncContext 决定: 继续 / 挂起 / 退出 / Flush
```

带内信令（Flush / EOF）仍然以"flag Packet / Frame"形式沿数据通道顺序传播，保证与数据流时序一致：

- **Flush**：Seek 时由 Demuxer 在新位置数据之前注入一个 `kFlush` flag 的空 Packet；每个下游节点遇到时清空自身内部状态再继续
- **EOF**：Demuxer 读到 EOF 时注入 `kEOF` flag；末端节点（AVSync / Speaker）收到后回调 Pipeline 完成

---

## 技术栈

| 技术领域 | 选型方案 |
|----------|----------|
| 核心开发语言 | C/C++ 17 |
| 媒体处理框架 | FFmpeg 6.x（libavcodec / libavformat / libavutil / libswscale / libswresample） |
| 音频后端 | SDL2 |
| 界面与跨平台框架 | Qt 6.x |
| 视频渲染引擎 | OpenGL 3.3 Core / EGL（硬件零拷贝） |
| 日志 | spdlog |
| 构建工具 | CMake 3.23+ |
| 依赖管理 | Conan 2 |
| 主开发环境 | Ubuntu 24.04 |
| 版本控制 | GitLab |

---

## 环境搭建

### 系统要求
- 推荐系统：Ubuntu 24.04 LTS
- 兼容系统：Windows 10+、macOS 12+
- 编译器：GCC 12+ / Clang 18+ / MSVC 2022
- 构建工具：CMake 3.23 及以上版本

### 依赖安装（Ubuntu 24.04）
```bash
# 更新软件源
sudo apt update && sudo apt upgrade -y

# 安装 Qt 6 开发套件
sudo apt install -y qt6-base-dev qt6-multimedia-dev qt6-tools-dev

# 安装 OpenGL / EGL 开发环境（含硬件解码零拷贝所需的 EGL 头）
sudo apt install -y libgl1-mesa-dev libegl1-mesa-dev libglu1-mesa-dev

# 安装构建工具与编译器
sudo apt install -y cmake build-essential gdb git pkg-config
```

> FFmpeg / spdlog / SDL2 / stb / glm 由 Conan2 管理，无需手动安装系统包

---

## 编译与运行

```bash
# 1. 克隆项目
git clone https://git.cpptrain.top/zhanmeng/ai-media-player.git
cd ai-media-player

# 2. 一键构建
./build.sh

# 3. 运行
./build/Release/ai_media_player
```

---

## 文档导航

| 文档 | 说明 |
|-----|------|
| [README.md](./README.md) | 项目总览与架构设计（本文件） |
| [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md) | 重构 + 演进路线的详细开发计划书 |

---

## 开发规范

### 一、分支管理规范

| 分支类型 | 命名规范 | 用途说明 |
|----------|----------|----------|
| 主分支 | `main` | 仅存放稳定、可运行的代码，禁止直接提交，仅接受 MR 合并 |
| 重构分支 | `refactor/<scope>` | 架构性改造，如 `refactor/dag` |
| 功能分支 | `feat/<模块名-功能描述>` | 新功能、新模块开发，如 `feat/flv-demuxer` |
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
feat(flv-demuxer): 实现 FLV Script Tag 解析

- 解析 AMF0 元数据，提取 width/height/duration/framerate
- 兼容 OBS 推流与 B 站下载的 flv 文件
- 添加 3 个真实样本作为回归测试

Closes #12
```

---

## 项目已实现功能
- 项目框架搭建、CMake 跨平台构建配置（Conan2 依赖管理）
- 文件解封装、音视频解码、音画同步、音视频渲染播放全链路
- 基础播放控制：播放 / 暂停 / 进度拖动 / Seek
- 音视频录制：H.264 + AAC 编码、MP4 封装
- 基础音视频效果：音频均衡、画面滤镜

> 重构进度与未来路线见 [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md)

---

## 许可证
本项目仅供学习与研究使用，未经授权禁止用于商业用途

---

## 问题反馈
如有问题、建议或 Bug 反馈，可通过 GitLab Issue 提交


