# media_player

## 项目简介

`media_player` 是面向 Linux 桌面的 C++17 音视频播放器。项目使用 FFmpeg 进行媒体输入、解复用和解码，以 Qt 6 提供桌面界面，并通过独立的 `mediacore` 库组织播放控制、音视频同步和输出。核心媒体处理不依赖 Qt 界面对象。

## 核心功能模块

- **媒体输入与解复用**：使用 FFmpeg 打开媒体文件，探测并选择音视频流，将压缩数据包送入播放流水线。
- **视频解码与渲染**：支持 FFmpeg 软件解码；在设备和解码器可用时可选择 VA-API。软件帧通过 OpenGL 纹理显示。VA-API 硬件帧可尝试导出 DMA-BUF、导入 EGLImage 并绑定 OpenGL Texture；不支持或导入失败时回退到 CPU 下载和软件纹理渲染。
- **音频解码与输出**：通过 FFmpeg 解码音频，使用 `swr` 转换为输出格式，再经 ALSA PCM 设备播放。
- **音视频同步**：以音频设备时钟调度视频；音频时钟不可用时使用单调时钟。Seek 使用 epoch 和 `StreamReset` 隔离旧数据，并刷新解码状态。
- **播放控制**：支持打开文件、播放、暂停、恢复、Seek 和停止。正常 EOS 会排空解码器延迟帧及音频设备缓冲。
- **运行统计**：日志记录解码模式、帧处理和同步耗时、视频 sink 交付速率等信息。帧率统计为墙钟区间值，受暂停、同步和队列背压影响，不代表解码器最大吞吐量。

VA-API、DMA-BUF/EGL 导入和真实音频输出依赖主机设备及驱动。代码包含相应路径，不代表该路径已在所有主机验证成功；驱动也可能在导入内部进行复制，因此不能仅凭 EGL 导入成功认定为零拷贝。

## 项目架构与数据流

```text
PlayerWidget --IPlayer commands--> Player::Control --> OpenTask --> Pipeline
      ^                                                        |
      | IVideoSink frames                                      v
  OpenGLView <-- VideoSinkNode <-- AvSyncNode <-- VideoDecoderNode
                                                       ^
                                                       |
                                                DemuxerNode
                                                       |
                      ALSA <-- AudioSinkNode <-- AudioDecoderNode + swr
```

打开任务在独立线程中探测输入并初始化解码器；控制线程随后按文件中实际存在的轨道组装 Pipeline。Pipeline 中的 Node 在各自工作线程运行，通过有界 `Link<T>` 传递压缩包、解码帧、PCM 和流控制项。队列满时产生背压；EOS 沿流水线排空传递，Seek 则以新 epoch、`StreamReset` 和解码器 Flush 切换播放周期。

更详细的对象所有权、线程协议、Seek/EOS 和硬件帧生命周期见 [ARCHITECTURE.md](./ARCHITECTURE.md)。

## 技术栈

- C++17、CMake 3.23 或更新版本
- Conan 2 管理依赖及 CMake toolchain
- FFmpeg 8.0.1、spdlog 1.15.3、GLM
- Qt 6.7.3：Core、Gui、Widgets、OpenGL、OpenGLWidgets
- OpenGL/EGL、libva/VA-API、DRM
- ALSA PCM

## 环境搭建与依赖安装

以下命令以 Ubuntu/Debian 为例。Conan 管理 FFmpeg、spdlog、GLM 和 Qt；ALSA、EGL/OpenGL 及 VA-API/DRM 开发文件由系统提供。VA-API 运行还需要兼容的驱动和可访问的 `/dev/dri/renderD*` 设备；播放音频需要实际可用的 ALSA 设备。

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config python3-pip \
  libasound2-dev libegl-dev libgl-dev libva-dev libdrm-dev
python3 -m pip install --user 'conan>=2.0'
```

### 克隆项目

```bash
git clone https://git.cpptrain.top/zhanmeng/ai-media-player.git
cd ai-media-player
```

### 安装 Conan 依赖并配置 CMake

项目使用 `conan.lock` 固定依赖版本。Conan 安装会生成对应的 CMake preset 和运行环境脚本：

```bash
conan install . --lockfile=conan.lock --build=missing \
  -s build_type=RelWithDebInfo
cmake --preset conan-relwithdebinfo
```

首次配置可能需要 Conan 下载或构建依赖。使用 `--build=missing` 时，若本机没有匹配的二进制包，Conan 会从源码构建。

播放器要求 Qt 的 `with_egl=True` 和 XCB EGL integration。修改依赖选项后，须重新运行上述 Conan 安装和 CMake 配置；只执行 `cmake --build` 不会重新解析 Conan 依赖。CMake 会编译并链接检查所选 Qt 的 EGL 特性及 `QEGLContext`，不满足时停止配置。不要手工指定 Conan 缓存包路径或混用不同包的 Qt 头文件、库和插件。

### 编译与运行

```bash
cmake --build --preset conan-relwithdebinfo --parallel 4
source build/RelWithDebInfo/generators/conanrun.sh
./build/RelWithDebInfo/app/media_player [--video-decoder=auto|software|vaapi] [media-file]
```

不传媒体文件时可在界面中打开文件；传入文件路径时程序启动后自动打开。解码模式默认为 `auto`：优先尝试 VA-API，在初始化或格式协商阶段不可用时回退软件解码。`software` 强制软件解码；`vaapi` 强制 VA-API，初始化失败时打开失败。

`build.sh` 可执行 Conan 安装、CMake 配置和增量编译，并保留现有 `build/` 和用户预设。

在没有 Qt Wayland 插件的桌面上，可仅为播放器进程选择 XWayland 和 EGL：

```bash
QT_QPA_PLATFORM=xcb QT_XCB_GL_INTEGRATION=xcb_egl \
  ./build/RelWithDebInfo/app/media_player
```

启动日志会记录实际 QPA 平台、EGL/GLX Context 和 DMA-BUF 格式查询结果。选择环境变量不等于 EGL 初始化成功，应以运行日志为准；不传媒体文件即可检查 Context，不执行视频帧导入。

运行日志默认写入 `logs/media_player.log`。测试可使用 CTest：

```bash
ctest --test-dir build/RelWithDebInfo --output-on-failure
```

单元和集成测试不能替代真实桌面显示、ALSA 输出、VA-API 或 DMA-BUF/EGL 渲染的设备测试。报告运行结果时，应注明所用媒体、解码路径和设备条件；未实际运行的功能应标为未验证。

## 项目目录

```text
media_player/
├── app/                              # Qt 桌面界面和 OpenGL 渲染
│   ├── controller_widget.*           # 播放控制面板
│   ├── player_widget.*               # 播放窗口及 IPlayer/IVideoSink 桥接
│   ├── opengl_view.*                 # OpenGLView、EGL 导入和帧资源回收
│   └── deferred_resource_queue.h     # 延迟释放 GPU 资源队列
├── mediacore/
│   ├── include/mediacore/            # 对界面层公开的播放器接口和媒体类型
│   │   ├── av_types/                 # 音视频帧、流信息和硬件帧类型
│   │   ├── iplayer.h
│   │   ├── ivideo_sink.h
│   │   ├── player_config.h
│   │   └── logger.h
│   └── src/                          # 播放核心实现
│       ├── audio_decoder/            # FFmpeg 音频解码、重采样和 ALSA sink
│       ├── av_sync/                  # 播放时间轴与视频同步
│       ├── av_types/                 # 内部媒体类型实现
│       ├── config/                   # 配置实现
│       ├── core/                     # Player、Pipeline、Node、Link 和线程管理
│       ├── demux/                    # FFmpeg 输入、选流和解复用
│       ├── sink/                     # 视频交付、音频消费和时钟
│       ├── utils/                    # 日志、错误和线程安全队列
│       └── video_decoder/            # FFmpeg 软件/VA-API 视频解码
├── resource/                         # 静态资源和测试媒体
│   ├── pack/
│   ├── sticker/
│   └── video/
├── tests/                            # DAG、核心、解复用、解码和应用测试
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── dag/
│   ├── decode/
│   └── demux/
├── cache/                            # Conan 源码下载缓存，运行构建时生成
├── logs/                             # 运行日志
├── CMakeLists.txt
├── conanfile.py
├── conan.lock
├── build.sh
├── README.md
└── ARCHITECTURE.md
```

## 开发规范

### 分支管理

| 分支类型 | 命名规范 | 用途 |
|---|---|---|
| 主分支 | `main` | 稳定代码；不直接提交，通过 MR 合并 |
| 功能分支 | `feature/<模块-功能>` | 开发新功能，例如 `feature/demux-mkv-support` |
| 修复分支 | `hotfix/<问题描述>` | 紧急问题修复，例如 `hotfix/fix-av-sync-drift` |
| 发布分支 | `release/v<版本号>` | 版本发布前测试与修复，例如 `release/v1.0.0` |
| 文档分支 | `docs/<文档内容>` | 文档更新，例如 `docs/update-architecture-doc` |

### Commit 消息

```text
<type>(<scope>): <subject>

<body>

<footer>
```

`type` 必填，`scope`、`body` 和 `footer` 可选。`subject` 简洁描述变更，建议不超过 50 个字符。`body` 说明目的和实现，`footer` 可关联 Issue 或标注不兼容变更。

| type | 用途 |
|---|---|
| `feat` | 新增功能 |
| `fix` | 修复缺陷 |
| `docs` | 文档更新 |
| `style` | 不改变逻辑的格式调整 |
| `refactor` | 不新增功能或修复缺陷的代码重构 |
| `test` | 新增或修改测试 |
| `chore` | 构建、辅助工具或依赖变更 |

示例仅用于说明格式：

```text
fix(av_sync): 修复 Seek 后时间轴未重置

- 重置新播放周期的时钟基准
- 补充 Seek 后帧时间戳回归测试

Closes #12
```

### 开发流程

1. 从 `main` 创建 `feature/<feature-name>` 分支。
2. 在功能分支上开发并运行相关测试。
3. 按 Commit 规范提交到远程分支。
4. 向 `main` 发起 MR 并完成代码审查。
5. 审查通过后合并，并删除已完成的功能分支。

### 发布流程

1. 从 `main` 创建 `release/<version>` 分支。
2. 在发布分支上进行集成测试和必要修复。
3. 验证通过后合并到 `main`，并创建对应版本 Tag。

### 修复流程

1. 从 `main` 创建 `hotfix/<issue-name>` 分支。
2. 完成问题修复和相关自测。
3. 按 Commit 规范提交到远程分支。
4. 向 `main` 发起 MR 并完成代码审查。
5. 审查通过后合并，并删除已完成的修复分支。

## 文档

- [ARCHITECTURE.md](./ARCHITECTURE.md)：组件、线程、数据流和资源生命周期。
- [tests/dag/README.md](./tests/dag/README.md)：DAG 测试说明。
