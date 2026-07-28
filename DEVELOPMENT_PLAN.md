# 开发计划书 —— 《30 天从 0 到 1 架构师带练计划》

> 从 0 手写重构一个工业级跨平台音视频播放器。工程哲学：**自顶向下（先见黑框 → 再挖内核 → 串本地 → 攻硬解 → 死磕网络）** + **先软解保底、后硬解加速**。

- **每日工作量**：8 小时/天 × 30 天
- **交互准则（导师带练）**：理论先行、代码辅助；每次只推进一个小模块；每个知识点跑通后再进入下一个
- **节奏原则**：每阶段独立 git 分支，合并 `main` 前必须冒烟回归通过

## 宏观架构硬底线（100% 覆盖）

1. **统一解封装**：废弃手写 FLV/MP4，统一 `libavformat` 兜底
2. **DAG 引擎**：泛型节点 `INode` + 强类型有界阻塞队列 `Link`（`condition_variable` 背压防 OOM）；控制信令用无锁 `atomic epoch` + 带内 `kFlush`
3. **软硬结合解码渲染**：先软解跑通，再 VA-API 硬解 → `vaExportSurfaceHandle` 导出 DMA-BUF → `eglCreateImageKHR` 零拷贝
4. **高级 GLSL 色彩**：BT.601/709/2020 矩阵动态切换 + PQ EOTF 反变换 + Reinhard HDR Tone Mapping
5. **网络 QoS 抗弱网**：HTTP/RTP 接入 + 手写 Jitter Buffer（乱序/空洞/自适应深度）+ GOP Cache 秒开(<180ms) + 非对称同步(±40ms)
6. **WebRTC 理论**：不集成源码，末期产出面试级白皮书（ICE/SRTP/GCC/FEC 等）

---

## 阶段一：外壳与基建（UI & Facade）· Day 1–5

> 目标：从 0 跑通 CMake/Conan，搭出 Qt 播放器空壳与黑色 OpenGL 画布，定义 UI 与底层唯一契约 `IPlayer`。用桩驱动打通"UI→接口→渲染"回环，获得视觉正反馈。

| Day | 今日目标 | 核心类/接口 | 技术难点 / 面试考点 |
|-----|---------|------------|-------------------|
| **D1** | 旧代码归档进 `legacy_v1/`；从 0 写 CMake + Conan；Qt 空窗口跑出黑框 | `CMakeLists.txt`、`conanfile.py`、`main.cpp`、`MainWindow` | CMake target 分层、Conan2 toolchain 集成、Qt6 `AUTOMOC`、`find_package` vs `pkg_check_modules` |
| **D2** | 定义 Facade 契约（纯头文件）：`IPlayer` + `IVideoSink` + `PlaybackState` | `iplayer.h`、`ivideo_sink.h`、`playback_state.h` | Facade 模式、依赖倒置(DIP)、接口隔离、前向声明降耦合 |
| **D3** | UI 骨架：`PlayerWidget`(持 `IPlayer`) + `ControllerWidget`(播放/暂停/进度条)，信号槽连通 | `PlayerWidget`、`ControllerWidget` | Qt signal/slot、`Qt::QueuedConnection` 跨线程投递、moc、UI 与逻辑解耦 |
| **D4** | `OpenGLView` 空画布：`QOpenGLWidget` + `initializeGL/paintGL` 渲染纯色 | `OpenGLView`、`QOpenGLFunctions` | GL 上下文线程亲和、双缓冲、`paintGL` 触发、渲染为何在 GUI 线程 |
| **D5** ✅ | `StubPlayer`(假 `IPlayer`，生成渐变测试帧)推给 `IVideoSink`，打通全回环 | `StubPlayer`、`IVideoSink::OnReceiveVideoFrame` | 桩/Mock 驱动开发、接口先行的可测性、生产者-消费者雏形 |

## 阶段二：DAG 引擎核心（Core）· Day 6–9

> 目标：手写 DAG 两大基石——有界阻塞队列 `Link`（背压防 OOM）与泛型节点基类 `INode`（线程生命周期）。

| Day | 今日目标 | 核心类/接口 | 技术难点 / 面试考点 |
|-----|---------|------------|-------------------|
| **D6** ✅ | 手写 `ThreadSafeQueue<T>`：双 CV + 背压 + `Abort` | `ThreadSafeQueue<T>`、`condition_variable` | 谓词 `wait` 防虚假唤醒、背压防 OOM、为何需两个 CV、Abort 唤醒退出、`std::move` |
| **D7** ✅ | 封装 `Link<T>`：语义化连接器 + `epoch` 感知预留 | `Link<T>` | 强类型 vs `void*` 编译期安全、模板、`shared_ptr` 所有权 |
| **D8** | `INodeBase`(Start/RequestStop/WaitForStop) + `INode<InT,OutT>` 模板 | `INodeBase`、`INode<InT,OutT>` | 模板方法模式、两阶段停止、线程 RAII、虚析构 |
| **D9** | `ThreadLoop` 三态骨架 + `SyncContext`(atomic state+epoch)；两节点串联单测 | `SyncContext`、`ThreadLoop` | 原子内存序、惊群、如何为并发写单测 |

## 阶段三：本地播放主干（Pipeline）· Day 10–17

> 目标：串起 Demux → 软解 → 同步 → 渲染，打通"文件到画面/声音"，落地 atomic epoch Seek。**先软解保底**。

| Day | 今日目标 | 核心类/接口 | 技术难点 / 面试考点 |
|-----|---------|------------|-------------------|
| **D10** | DTO：`Packet`(RAII 包 `AVPacket*` + pts/dts/flags/epoch) + `AVFrameFlag` | `Packet`、`AVFrameFlag` | `AVPacket` RAII、`enum class` 位运算重载、DTO 设计 |
| **D11** | `FfmpegDemuxerNode`：`avformat_open_input`/`find_stream_info`，独立线程 `av_read_frame`，双输出 | `FfmpegDemuxerNode` | 流探测、`codecpar`、`time_base` 换算微秒、多输出扇出 |
| **D12** | `VideoFrame` sum type(Software variant，保留 `linesize`) + `AudioFrame` | `VideoFrame`、`AudioFrame` | variant 设计、保留 linesize 免重打包、`shared_ptr` 自定义删除器 |
| **D13** | `VideoDecoderNode`(软解)：`send_packet`/`receive_frame` → `VideoFrame` | `VideoDecoderNode` | send/receive 状态机、EAGAIN/EOF、B 帧重排、`best_effort_timestamp` |
| **D14** | `AudioDecoderNode` + swr 重采样；定义 `IAudioSink` 接口 + `AlsaAudioSink`（直连 libasound，`snd_pcm_writei` 投递）；`AudioSinkNode` 实现 `IClockSource` | `AudioDecoderNode`、`IAudioSink`、`AlsaAudioSink`、`AudioSinkNode`、`IClockSource` | swr 重采样、ALSA buffer/period 参数、xrun 欠载恢复、阻塞 vs 非阻塞写、`snd_pcm_delay` 推算音频主时钟 |
| **D15** | `AVSyncNode`(注入 `IClockSource`，丢/等/送) + `VideoSinkNode` 推 UI | `AVSyncNode`、`VideoSinkNode` | 音画同步原理、主时钟选择、diff 计算、精确 sleep |
| **D16** | `Pipeline`(Build 组图、消费者先启、反向回收) + `PlayerFacade` + `FSM` | `Pipeline`、`PlayerFacade`、`FSM` | 拓扑构建、线程回收顺序防死锁、显式状态机合法跃迁表 |
| **D17** | atomic epoch Seek + 带内 `kFlush`/`kEOF`；播放/暂停/Seek/停止全回归 | `SyncContext.epoch_`、`kFlush` | 无锁世代号 vs 多标志握手、带内/带外信令、seek 在飞帧竞态、`avcodec_flush_buffers` |

## 阶段四：硬核零拷贝与 HDR（HW & GL）· Day 18–24

> 目标：**软解跑通后接硬解**。VA-API → DMA-BUF → EGLImage 零拷贝；GLSL 色彩矩阵 + HDR Tone Mapping。

| Day | 今日目标 | 核心类/接口 | 技术难点 / 面试考点 |
|-----|---------|------------|-------------------|
| **D18** | `HwDeviceContext` 封装(VAAPI) + `vainfo` 探测 | `HwDeviceContext` | `av_hwdevice_ctx_create`、VA-API 架构、`AVBufferRef` 引用计数 |
| **D19** | 解码器接硬解：`get_format` 返回 `AV_PIX_FMT_VAAPI`，Hardware variant 持 `AVBufferRef`；先 `transfer` 降级验证 | `VideoDecoderNode`(hw) | 格式协商、硬件像素格式、`av_hwframe_transfer_data` 降级、显存帧生命周期 |
| **D20** | `vaExportSurfaceHandle` 导出 DMA-BUF fd | `vaExportSurfaceHandle` | DMA-BUF 机制、`VASurfaceID`、多平面 offset/stride/modifier、fd 所有权 |
| **D21** | `HwTextureImporter`：`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` + `glEGLImageTargetTexture2DOES`，NV12 双纹理 | `HwTextureImporter` | EGLImage、零拷贝原理、NV12 双平面(R8/RG8)、EGL 扩展查询 |
| **D22** | `OpenGLView` 双路径分派(sw/hw) + 4K 压测对比 + 硬解失败 fallback | `OpenGLView`、`sw_texture_uploader` | 零拷贝收益量化、fallback 鲁棒、`intel_gpu_top` 观测 |
| **D23** | GLSL 色彩管线：BT.601/709/2020 矩阵 `uniform` 化，`color_space/color_range` 注入 | `color_shader.h` | YUV→RGB 矩阵、limited/full range、三大色域区别、为何在 shader 做 |
| **D24** | HDR：PQ EOTF 反变换 + Reinhard tone mapping | `color_shader`(HDR 分支) | PQ/HLG 传递函数、EOTF、线性光空间、tone mapping 算法 |

## 阶段五：弱网对抗与收尾（QoS + Theory）· Day 25–30

> 目标：HTTP/RTP 拉流，手写 Jitter Buffer（乱序/空洞/自适应）+ GOP 秒开，非对称同步 + QoS 监控，WebRTC 面试笔记收尾。

| Day | 今日目标 | 核心类/接口 | 技术难点 / 面试考点 |
|-----|---------|------------|-------------------|
| **D25** | `ISourceNode` 抽象 + `HttpSourceNode`(libcurl streaming + Range)；AVIOContext 自定义读回调 | `ISourceNode`、`HttpSourceNode` | AVIOContext 自定义 IO、libcurl streaming、Range、moov 位置与秒开 |
| **D26** | `RtpSourceNode`(UDP recv + RTP 头解析) + `RtpDepacketizerNode`(FU-A/STAP-A) | `RtpSourceNode`、`RtpDepacketizerNode` | RTP 头结构、H264 打包模式、UDP 无连接、NALU 组帧 |
| **D27** | `JitterBufferNode`：seq 乱序重排 + 空洞检测(超时判丢) | `JitterBufferNode` | seq wrap-around、乱序窗口、空洞超时、延迟↔流畅权衡 |
| **D28** | 自适应缓存深度(按 jitter 统计) + `GopCache` 首帧秒开(<180ms) | `JitterBufferNode`、`GopCache` | jitter 估计、动态 stretch/shrink、GOP/IDR、秒开策略 |
| **D29** | 非对称同步(±40ms) + `QoSMonitor`(卡顿率/丢帧率/延迟) + `FecDecoderNode`(XOR) | `AVSyncNode`(参数化)、`QoSMonitor`、`FecDecoderNode` | 非对称阈值、卡顿率定义、XOR FEC、FEC vs ARQ 取舍 |
| **D30** | WebRTC 白皮书(ICE/STUN/TURN/SRTP/GCC/BBR/FEC/ARQ) + 全链路回归矩阵 + 性能数据表 | `docs/webrtc-research.md` | WebRTC 数据流、拥塞控制原理、回归矩阵、面试话术 |

---

## 执行原则

1. 每阶段独立 git 分支，合并 `main` 前冒烟回归通过
2. 地基（阶段一/二/三）宁可慢不可省，卡壳从后续 buffer 借时间，不带病推进
3. 硬解只做透 Linux/VAAPI；macOS/Windows 不在本期
4. 每阶段产出一份 markdown 笔记（字段图/踩坑/性能数据），转化为面试材料
5. 性能以数据说话，禁止"感觉变快了"

## 进度看板

| 阶段 | 状态 | 开始 | 完成 | 备注 |
|-----|------|-----|-----|-----|
| 一：外壳与基建 (D1–5) | ✅ 已完成 | 2026-07 | 2026-07-27 | StubPlayer 生成渐变帧，打通 UI→IPlayer→IVideoSink→OpenGLView 渲染回环 |
| 二：DAG 引擎核心 (D6–9) | 🚧 进行中 | 2026-07-28 | – | D6 ThreadSafeQueue、D7 Link 已完成并通过验证 |
| 三：本地播放主干 (D10–17) | 未开始 | – | – | – |
| 四：硬核零拷贝与 HDR (D18–24) | 未开始 | – | – | – |
| 五：弱网对抗与收尾 (D25–30) | 未开始 | – | – | – |
