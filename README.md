# ai-media-player

基于 C++17 + FFmpeg + Qt 6 + OpenGL 的桌面音视频播放器工程。

---

## Overview

本项目是一款面向工程能力打磨的桌面播放器。媒体内核采用**强类型固定拓扑的节点图（DAG）**：每个媒体处理环节是一个 Node，Node 各自跑在独立线程上，节点之间通过有界阻塞队列包装成的 `Link<T>` 单向传递数据，队列满时自动产生背压，整张图的生命周期由 `Pipeline` 独占管理。

UI 层用 Qt 6 + OpenGL 实现，通过 `IPlayer` / `IVideoSink` 两个接口与媒体内核完全解耦：UI 不知道 Demuxer、Decoder、Pipeline 的存在，媒体内核也不认识 Qt。


---

## Goals & Scope

### Goals

按依赖顺序推进，前一项没有可复现的验收结果就不进入后一项：

1. **稳定 DAG** —— Queue / Link / Node / SyncContext / Pipeline 的生命周期、背压、暂停、停止和并发测试
2. **本地软件播放** —— 解封装、软件视频解码、软件渲染，形成最小播放闭环
3. **音频与同步** —— 音频解码、ALSA 输出、音频主时钟、视频同步
4. **Seek / EOF / 错误闭环** —— epoch 过期判定、带内 Flush、正常排空、结构化错误上报
5. **录制** —— Tee、编码、MP4 Mux 和独立 Drain 语义
6. **VA-API 与零拷贝** —— 硬解正确性与软件 fallback，再打通 DMA-BUF / EGLImage
7. **色彩管理** —— SDR 矩阵与范围、HDR 识别、HDR 到 SDR tone mapping
8. **HTTP 网络播放** —— 基于 FFmpeg 的 URL、超时和 interrupt callback
9. **RTP 接收 + Jitter Buffer** —— 可取消 UDP 接收、H.264 解包、固定重排判丢
10. **可复现弱网测试** —— 固定 trace / netem profile 验证首帧、rebuffer、延迟、丢包

### Non-Goals

- 不自写 FLV/MP4 等容器解析器，容器探测和解封装统一交给 FFmpeg
- 不做 RTSP / RTMP / SRT / QUIC
- 不集成完整 WebRTC 协议栈，不做 RTCP、SRTP
- 不做 RTP 发送侧：无 Pacer、无 GCC/BBR、无发送端动态码率
- 默认 RTP 路径不含 FEC，也不含 GOP Cache
- FFmpeg HTTP 能满足取消、超时和观测需求时，不增加自定义 HTTP 层
- 不修改运行中的主 Pipeline 拓扑
- 不承诺脱离测试条件的性能数字，性能结论必须附输入、环境和测量数据

---

## Project Structure

下面是项目完成时的目标结构，按 [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md) 的交付内容组织。当前仓库实现到哪一步，以源码和测试为准。

```text
ai-media-player/
├── CMakeLists.txt                      顶层构建入口
├── build.sh / clean.sh                 Conan + CMake 一键构建 / 清理
├── conanfile.py                        Conan 2 依赖清单
├── .clang-format / .clang-tidy         格式与静态检查规则
│
├── mediacore/                          媒体内核静态库（libmediacore）
│   ├── include/mediacore/              PUBLIC：app 层可见的契约与跨边界 DTO
│   │   ├── iplayer.h                   播放控制接口
│   │   ├── ivideo_sink.h               视频帧消费接口
│   │   ├── playback_state.h            播放状态枚举
│   │   ├── player_factory.h            CreatePlayer() 工厂
│   │   ├── logger.h                    日志宏与初始化接口
│   │   └── av_types/                   跨边界 DTO
│   │       ├── frame_size.h            具名宽高
│   │       ├── media_data_flag.h         帧/包带内标志位
│   │       └── video_frame.h           视频帧（Software / Hardware 两种形态）
│   │
│   └── src/                            PRIVATE：仅库自身可见
│       ├── core/                        DAG 框架：图所有权、线程生命周期、数据边、运行门控
│       │   ├── pipeline.h
│       │   ├── inode.h
│       │   ├── link.h
│       │   └── sync_context.h
│       │
│       ├── player/                     播放器门面与状态机
│       │   ├── player_facade.h/.cpp    实现 IPlayer，串行化控制命令
│       │   ├── fsm.h/.cpp              PlaybackState 合法跃迁表
│       │   └── player_factory.cpp
│       │
│       ├── config/
│       │   └── player_config.h         全部可调参数的唯一来源
│       │
│       ├── av_types/                   内部 DTO，只在 Node 之间流动
│       │   ├── packet.h/.cpp           压缩包
│       │   ├── audio_frame.h           PCM 帧
│       │   ├── stream_info.h           流信息探测结果
│       │   ├── media_error.h           结构化错误
│       │   ├── rtp_packet.h            RTP 包
│       │   └── byte_chunk.h            字节块（条件能力）
│       │
│       ├── source/                     零输入端口的数据源节点
│       │   ├── rtp_source_node.h/.cpp  可取消 UDP 接收
│       │   └── http_source_node.h/.cpp 自定义 HTTP 拉流（条件能力）
│       │
│       ├── rtp/                        RTP 协议处理
│       │   ├── rtp_depacketizer_node.h/.cpp  H.264 FU-A / STAP-A 组帧
│       │   └── jitter_buffer_node.h/.cpp     重排、判丢、有界自适应
│       │
│       ├── demux/
│       │   └── ffmpeg_demuxer_node.h/.cpp    解封装，本地路径与 HTTP URL
│       │
│       ├── decode/
│       │   ├── video_decoder_node.h/.cpp     软解 + VA-API 硬解
│       │   └── audio_decoder_node.h/.cpp     avcodec + swr 重采样
│       │
│       ├── hardware/                   硬件基础设施（解码与编码共用）
│       │   ├── hw_device_context.h/.cpp     硬件上下文与帧所有权
│       │   └── hw_capability.h/.cpp         硬件能力探测与支持矩阵
│       │
│       ├── sync/
│       │   ├── av_sync_node.h/.cpp           音频主时钟，墙钟 fallback
│       │   └── iclock_source.h               主时钟接口
│       │
│       ├── sink/                       终端输出层
│       │   ├── video_sink_node.h/.cpp        投递到 IVideoSink
│       │   ├── audio_sink_node.h/.cpp        消费 PCM，兼 IClockSource
│       │   ├── iaudio_sink.h                 音频后端抽象
│       │   └── alsa_audio_sink.h/.cpp        ALSA 实现
│       │
│       ├── routing/
│       │   └── tee_node.h              帧多播（录制分支）
│       │
│       ├── encode/
│       │   ├── video_encoder_node.h/.cpp     H.264 编码
│       │   ├── audio_encoder_node.h/.cpp     AAC 编码
│       │   └── rate_control_config.h         CBR / CRF / CQP 参数
│       │
│       ├── mux/
│       │   └── mp4_muxer_node.h/.cpp         MP4 交错写盘
│       │
│       ├── qos/
│       │   └── qos_monitor.h/.cpp      首帧、rebuffer、队列深度等指标
│       │
│       └── utils/
│           ├── thread_safe_queue.h     有界阻塞队列
│           └── logger.cpp              spdlog 封装实现
│
├── app/                                Qt GUI 应用层
│   ├── main.cpp                        Qt 事件循环入口
│   ├── player_widget.h/.cpp            UI 信号 → IPlayer 调用
│   ├── controller_widget.h/.cpp        播放控制条
│   └── render/                         OpenGL 渲染
│       ├── opengl_view.h/.cpp          实现 IVideoSink，OpenGL 显示
│       ├── sw_texture_uploader.h/.cpp  软件 YUV 三平面上传
│       ├── hw_texture_importer.h/.cpp  DMA-BUF / EGLImage 导入
│       └── color_shader.h              SDR 色彩管理 + HDR Tone Mapping
│
├── tests/                              正式 CTest 用例，镜像 src 组织
│   ├── dag/                            DAG 框架生命周期与并发
│   ├── av_types/                       DTO 所有权与移动语义
│   ├── player/                         FSM 与播放器门面
│   ├── demux/                          解封装与时间戳
│   ├── decode/                         解码帧数、PTS、drain
│   ├── rtp/                            RTP 解包与 Jitter Buffer
│   ├── record/                         录制与 Drain
│   ├── network/                        HTTP 缓冲与重连
│   └── color/                          色彩管线参考比对
│
├── resource/                           测试媒体与资源文件
```

各目录职责：

| 目录 | 职责 |
|---|---|
| `mediacore/include/` | 公共契约。`app` 层唯一能 include 的内容，边界由 CMake 的 PUBLIC/PRIVATE 强制 |
| `mediacore/src/core/` | DAG 框架：图的所有权、线程生命周期、数据边、运行门控 |
| `mediacore/src/player/` | 播放器门面、状态机与工厂：命令校验、状态维护 |
| `mediacore/src/config/` | 配置系统：全部可调参数的唯一来源 |
| `mediacore/src/av_types/` | 内部 DTO，只在 Node 之间流动，UI 不可见 |
| `mediacore/src/` 下各媒体目录 | 按处理环节分类：source（数据源）、rtp（RTP 处理）、demux（解封装）、decode（解码）、hardware（硬件）、sync（同步）、sink（输出）、routing（路由）、encode（编码）、mux（封装）、qos（指标） |
| `mediacore/src/utils/` | 与媒体无关的并发原语与日志实现 |
| `app/` | Qt 窗口、控制条和 OpenGL 渲染，不含任何媒体处理逻辑 |
| `tests/` | 正式 CTest 用例，镜像 `src` 的模块划分 |

`byte_chunk.h` 与 `http_source_node` 是条件能力：只有 Day 40 的决策门证明 FFmpeg 的 URL 输入无法满足需求时才落地，默认不启用。`media_error.h`、`rtp_packet.h`、`hw_capability.h` 对应开发计划中已明确要求但未指定文件名的交付物，最终命名以实现为准。

---

## Architecture

### 高层结构

```text
              Qt UI (app/)
                   │  Play / Pause / Stop / Seek
                   ▼
              IPlayer 接口
                   │
                   ▼
               Pipeline            拥有整张 DAG，负责启停与 Join
                   │
      ┌────────────┼────────────┐
      ▼            ▼            ▼
    Source       Node...       Sink       各自独立线程
      │            │            │
      └── Link<T> ─┴── Link<T> ─┘         强类型有界队列 + 背压
                   │
              SyncContext                 运行/暂停门控 + epoch
```

### 核心组件

| 组件 | 职责 |
|---|---|
| `IPlayer` | UI 可见的播放控制契约：Open / Play / Pause / Stop / Seek / GetState |
| `IVideoSink` | 视频帧消费接口，内核通过它把帧交给 UI |
| `Pipeline` | 创建并登记全部 Node 和 Link，按固定顺序启动、暂停、停止和 Join 整张图 |
| `INodeBase` / `INode<InT, OutT>` | 单个 Node 的线程循环与停止协议；端口模板提供强类型的输入输出连接 |
| `Link<T>` | 连接两个 Node 的单向数据边，负责正常 EOF（Close）与立即中止（Abort） |
| `ThreadSafeQueue<T>` | 有界阻塞队列，提供背压、Close 排空和 Abort 丢弃 |
| `SyncContext` | 一张图内全部 Node 共享的运行/暂停状态和 Seek 世代号（epoch） |
| `Packet` / `VideoFrame` | 在 Node 之间流动的媒体数据对象 |

### 设计原则

- UI 不实现任何媒体处理，只发命令和显示画面
- `Pipeline` 独占媒体处理图的所有权和生命周期
- Node 只负责一次具体的媒体处理，不创建 Link，不知道完整拓扑，不管理自己的线程生命周期
- 媒体数据只通过强类型 `Link<T>` 单向流动，Node 之间不互相持有指针、不用回调链互相驱动
- 线程与生命周期管理和具体媒体处理逻辑彻底分离
- 一条 Link 默认一个生产者、一个消费者；广播必须由 Tee 向多条 Link 分别发送

实现层面的协议细节（停止顺序、EOF 与 Abort 的区别、所有权规则、编译期约束）见 [ARCHITECTURE.md](./ARCHITECTURE.md)。

---

## Data Flow

### 目标数据流（Target Architecture）

```text
本地文件 / 网络流
       │
       ▼
    Demux                   解封装，读出压缩包
       │
    Packet                  { AVPacket*, pts_us, dts_us, epoch, flags }
       │
       ▼
    Decode                  软解 / 硬解
       │
  ┌────┴────┐
  ▼         ▼
VideoFrame  AudioFrame
  │         │
  ▼         ▼
 Sync ◄─────┤            音频主时钟驱动视频同步
  │         │
  ▼         ▼
VideoSink  AudioSink
  │         │
  ▼         ▼
OpenGL     ALSA
```

### 当前已实现（Currently Implemented）

真实媒体节点尚未开发。当前可运行的数据流是桩实现：

```text
StubPlayer  ──生成渐变测试帧──►  IVideoSink  ──►  OpenGLView  ──►  屏幕
```

上图中的 Demux / Decode / Sync / Sink 节点**均未实现**。详见 [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md)。

---

## Technology Stack

| 领域 | 选型 | 状态 |
|---|---|---|
| 语言标准 | C++17（Google C++ Style Guide） | 使用中 |
| 构建 | CMake 3.23+ | 使用中 |
| 依赖管理 | Conan 2 | 使用中 |
| GUI | Qt 6.7 | 使用中 |
| 渲染 | OpenGL 3.3 Core（QOpenGLWidget） | 使用中 |
| 媒体框架 | FFmpeg 8.0.1（Conan 管理的 libav*，项目不链接系统 FFmpeg） | 部分接入 |
| 测试 | CTest | 使用中 |
| 动态检查 | ASan / UBSan / TSan | 使用中 |
| 日志 | spdlog | 使用中 |
| 数学工具 | glm | 已声明依赖，尚未接入代码 |
| 音频后端 | ALSA（libasound） | Planned |
| 硬件解码 | VA-API | Planned |
| 零拷贝 | DMA-BUF / EGLImage | Planned |
| 色彩管线 | GLSL 矩阵切换 + Tone Mapping | Planned |
| 网络 | FFmpeg HTTP、原生 UDP + RTP | Planned |

主开发环境：Ubuntu 26.04.1。

项目采用“两层依赖”模型：项目链接的 FFmpeg 8.0.1、Qt 6.7.3、spdlog
和 glm 来自 Conan；系统 apt 只提供主机能力和可选的命令行验收工具。
Conan 的 FFmpeg 配置为 `with_programs=False`，因此它不会安装
`ffmpeg`/`ffprobe` 可执行文件，也不会让 CMake 链接 `/usr/lib` 中的系统
`libav*`。完整构建必须使用 Conan 生成的 CMake preset，这样 CMake 才会从
`build/<配置>/generators/` 读取项目依赖。

| 层 | 由谁提供 | 作用 |
|---|---|---|
| 系统 apt / 主机 | `ffmpeg`（含 `ffprobe`，可选）、`libva-dev`、VA-API 驱动与运行时、GPU 驱动、`/dev/dri/renderD*` | CLI/样本验收、VA-API 设备和驱动；这些不属于 Conan 的 FFmpeg 库 |
| 项目 Conan | FFmpeg 8.0.1（静态 `libav*`，含 VA-API + libdrm PRIME 支持）、Qt 6.7.3、spdlog 1.15.3、glm | 项目编译与链接的唯一依赖来源 |
| 项目源码的系统接口 | ALSA、EGL/OpenGL、X11/Wayland、DRM 设备 API（按对应阶段安装开发包） | 音频、窗口系统和 Day 31–35 零拷贝路径 |

在 Ubuntu 上准备基础开发环境和本机 Intel VA-API 验证工具时，可执行：

```bash
sudo apt install build-essential cmake pkg-config pipx curl \
      libva-dev libdrm-dev libasound2-dev libegl-dev libgl-dev \
      ffmpeg vainfo intel-media-va-driver
pipx install conan
conan profile detect --force
```

`ffmpeg` 仅用于 `/usr/bin/ffmpeg`、`/usr/bin/ffprobe` 对照测试；项目不会链接
它的库。`libva-dev` 由 Conan 的 `vaapi/system` 依赖在构建 FFmpeg 时使用；
FFmpeg 的 `libdrm` 构建依赖由 Conan 锁定，`libdrm-dev` 主要供后续源码直接
包含 DRM 头文件的零拷贝阶段使用。VA-API 驱动、Intel/其他 GPU 用户态驱动和
`/dev/dri` 必须在运行主机上存在。Qt 还会通过 Conan 的 `xorg/system`、
`opengl/system` 等 recipe 检查窗口系统开发包；缺少的具体 apt 包会在首次
`conan install` 时列出。非 Intel GPU 应将上述 VA 驱动换成对应硬件的驱动。
当前开发机已检测到 Ubuntu 26.04.1、x86_64 和 Intel `i915`，但尚未安装
`ffmpeg`/`ffprobe` 命令，也没有可见的 `/dev/dri`，因此目前只能验证依赖图和
DAG-only 测试构建，不能宣称完整播放器或硬解运行时已验收。

---

## Build & Test

### 完整构建（需要 Qt 与 FFmpeg）

```bash
./build.sh                  # 默认 RelWithDebInfo，可传 Release / Debug
source build/RelWithDebInfo/generators/conanrun.sh
./build/RelWithDebInfo/app/media_player
```

`build.sh` 依次执行带 `conan.lock` 的 `conan install`、`cmake --preset` 和
`cmake --build`，每次运行会先清空 `build/`。锁文件固定 FFmpeg 8.0.1 的
recipe revision 和传递依赖；如果修改 `conanfile.py`，应重新生成并审阅锁文件。
由于 fontconfig 官方下载站会对非浏览器请求返回 HTTP 418，脚本会从 Debian 或
MacPorts 镜像预取内容完全相同的 2.15.0 归档，校验 recipe 指定的 SHA-256 后存入
`cache/conan-sources/`。这不会改变 Conan 锁定的 recipe、版本或源码内容。
从当前 recipe 重新生成（不消费旧锁）可执行：

```bash
conan lock create . --lockfile="" --lockfile-out=conan.lock \
      -s build_type=RelWithDebInfo
```

当前配置关闭了外部 GPL/nonfree 编码器，因为本轮只实现播放和硬解路径；开发
计划中的录制分支（H.264/AAC/MP4）未来需要单独选择编码器、许可证和对应 Conan
profile，不能把当前裁剪配置当作完整录制配置。

### 只构建并运行 DAG 测试（只需要 Conan 的 spdlog，不需要 Qt 与 FFmpeg）

```bash
conan install --requires=spdlog/1.15.3 --lockfile=conan.lock \
      --output-folder=build-dag/generators \
      --generator=CMakeDeps --generator=CMakeToolchain \
      --build=missing -s build_type=RelWithDebInfo
cmake -S . -B build-dag \
      -DMEDIA_PLAYER_BUILD_MEDIACORE=OFF \
      -DMEDIA_PLAYER_BUILD_APP=OFF \
      -DBUILD_TESTING=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_TOOLCHAIN_FILE=build-dag/generators/conan_toolchain.cmake
cmake --build build-dag -j
ctest --test-dir build-dag --output-on-failure
```

测试说明和 Sanitizer 运行方式见 [tests/dag/README.md](./tests/dag/README.md)。

---

## Current Status

| 里程碑 | 内容 | 状态 |
|---|---|---|
| D1–D5 | Qt 外壳、公共契约、OpenGL 渲染闭环、StubPlayer | 完成 |
| D6–D9 | DAG 核心：Queue / Link / Node / SyncContext / Pipeline | 完成 |
| D10 | `Packet` RAII、微秒时间戳、epoch、flags | 完成 |
| D11 | StreamInfo 转换（FFmpeg 对象 → `MediaInfo`） | 完成 |
| D12 | `FfmpegDemuxerNode` 打开、探测与读包 | 下一步（实现待审，见 `_review_temp/`） |
| D13 及之后 | FFmpeg 生命周期规则、解码、音频、同步、Seek、录制、硬解、色彩、网络 | 未开始 |

真实媒体解封装、解码、音画同步、录制、硬解和网络能力**均未实现**，当前画面来自 `StubPlayer` 的测试帧。

以 [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md) 为准；本表只是摘要，冲突时以开发计划为准。

---

## Documentation

| 文档 | 负责回答 |
|---|---|
| [README.md](./README.md) | 项目是什么、边界在哪、代码在哪、高层架构是什么、怎么构建 |
| [ARCHITECTURE.md](./ARCHITECTURE.md) | 已实现组件的详细设计、并发协议、生命周期和实现原理 |
| [DEVELOPMENT_PLAN.md](./DEVELOPMENT_PLAN.md) | 当前进度、已完成项、尚不存在的内容、验收标准和下一步 |
| [AI_DEVELOPMENT_GUIDELINES.md](./AI_DEVELOPMENT_GUIDELINES.md) | AI 协作方式、代码规范与注释要求 |
| [tests/dag/README.md](./tests/dag/README.md) | DAG 测试的构建、运行和覆盖范围 |

---

## Contributing

### 分支命名

| 类型 | 命名 | 用途 |
|---|---|---|
| 主分支 | `main` | 稳定可运行代码，仅接受 MR 合并 |
| 重构 | `refactor/<scope>` | 架构性改造 |
| 功能 | `feat/<模块-描述>` | 新功能、新模块 |
| 修复 | `fix/<问题描述>` | Bug 修复 |
| 文档 | `docs/<内容>` | 文档更新 |

### Commit 格式

```text
<type>(<scope>): <subject>

<body>
```

`type` 取值：`feat` / `fix` / `docs` / `style` / `refactor` / `test` / `chore`。
