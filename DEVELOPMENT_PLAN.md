# 开发计划书

本文件描述 **ai-media-player** 项目从当前"上帝对象 + Listener 回调"架构迁移到 **强类型固定拓扑 DAG** 架构，并叠加 6 个进阶能力（自写 FLV/MP4 demuxer、硬件解码零拷贝、码率控制、色彩空间/HDR、HTTP 拉流、QoS 调研）的完整路线。

- **每日工作量假设**：6 小时/天
- **目标周期**：约 14~15 周（含一个备选调研阶段）
- **节奏原则**：每阶段独立 git 分支，合并 main 前必须通过冒烟测试

---

## 总览

| 阶段 | 周期 | 累计 | 内容主线 | 分支命名 |
|-----|-----|-----|---------|---------|
| 阶段 1 | 14 天 | D14 | DAG 框架 + 现有模块迁移 + 拆 Player | `refactor/dag` |
| 阶段 2 | 5 天 | D19 | 抽 ISource，分离文件读取与解封装 | `refactor/source-layer` |
| 阶段 3 | 12 天 | D31 | 自写 FLV Demuxer | `feat/flv-demuxer` |
| 阶段 4 | 16 天 | D47 | 硬件解码 + 零拷贝渲染 | `feat/hw-decode` |
| 阶段 5 | 5 天 | D52 | 码率控制参数化（CBR/CRF/CQP） | `feat/rate-control` |
| 阶段 6 | 6 天 | D58 | 色彩空间矩阵 + HDR Tone Mapping | `feat/color-hdr` |
| 阶段 7 | 14 天 | D72 | 自写 MP4 Demuxer + HTTP Source | `feat/mp4-http` |
| 阶段 8 | 备选 | – | WebRTC / QoS 调研笔记（不实现） | `docs/webrtc-research` |

**强制要求**：阶段 1 + 2 + 3 必须按顺序完成，是后续所有阶段的地基。阶段 4 之后哪个先做都行。

---

## 阶段 1：DAG 框架与模块迁移（D1–D14）

**目标**：把回调链全部替换为强类型 Link 连接，让 Player 退化为 Facade，但**对外功能不退化**。

**里程碑**：本地 mp4 播放 / 暂停 / Seek / 录制全部回归通过，旧代码（FileReader、所有 Listener 接口）全部删除。

| 天 | 工作内容（6h） | 产出 |
|---|--------------|-----|
| D1 | 设计 `INode<InT, OutT>` 模板、`Link<T>` 封装、`INodeBase` 共同接口；编写最简单单测验证连接 | `core/inode.h`、`core/link.h` |
| D2 | 重写 `Packet`：加 `codec_id_` / `pts_us_` / `dts_us_` / `frame_kind_` / `flags_` / `stream_`；批量替换所有调用点 | `av_types/packet.h` |
| D3 | 重写 `VideoFrame` 为 sum type（先只实现 Software variant，预留 Hardware 接口）；同步整理 `AudioFrame` | `av_types/video_frame.h`、`audio_frame.h` |
| D4 | 把 `Demuxer` 改为 `FfmpegDemuxerNode`：输入 url 参数，双输出（video / audio）Link，删 Listener | `demux/ffmpeg_demuxer_node.*` |
| D5 | 把 `VideoDecoder` 改为 `VideoDecoderNode`：输入 Packet Link，输出 VideoFrame Link | `decode/video_decoder_node.*` |
| D6 | 同步迁移 `AudioDecoderNode`；写单元测试：用 FFmpeg 直接构造 Packet 喂入节点 | `decode/audio_decoder_node.*` |
| D7 | 实现通用 `TeeNode<T>`（含 enable 开关）；写 Tee 单测 | `filter/tee_node.h` |
| D8 | 把 `AVSynchronizer` 改为 `AVSyncNode`：引入 `IClockSource` 接口注入音频时钟 | `sync/av_sync_node.*`、`core/clock.h` |
| D9 | 把 `SDLAudioSpeaker` 改为 `SdlAudioSpeakerNode`，并 implement `IClockSource` | `output/sdl_audio_speaker_node.*` |
| D10 | 写 `Pipeline` 类：`Build` / `Start` / `RequestStop` / `WaitForStop` / `Seek`，反向回收线程 | `core/pipeline.*` |
| D11 | 把状态机从 Player 拆出，独立 `FSM` 类，显式定义合法跃迁表 | `core/fsm.*` |
| D12 | 写 `PlayerFacade`：组合 Pipeline + FSM + SyncContext，对 UI 实现 `IPlayer` 接口 | `core/player_facade.*` |
| D13 | 删除 `FileReader`、删除所有 Listener 接口；UI 持 `IPlayer`（不变） | 旧代码全删 |
| D14 | 全链路冒烟：本地 mp4 播放 / 暂停 / Seek / 录制 全部回归测试；修线程退出竞态 | 通过测试 |

### 关键决策
- **强类型优先**：节点之间 `Link<Packet>` / `Link<VideoFrame>` 编译期分隔，禁止用 `void*` 当通用 buffer
- **节点不互相持有指针**：只持有"下游输入队列"的 `shared_ptr`，节点之间编译期解耦，可独立单测
- **录制 PTS 修复**：本阶段顺手把"录制 PTS 自增"改为"复用解码器真实 pts_us"，写盘时再 rescale

---
## 阶段 2：Source 节点抽象（D15–D19）

**目标**：把"如何拿到字节"和"如何解析容器"两件事彻底分离，让后续 FLV / MP4 / HTTP / RTP 全部能复用同一个 Demuxer 抽象层。

**里程碑**：本地文件链路已变成 "FileSource 节点 → FfmpegDemuxer 节点" 两段式，UI 无感知。

| 天 | 工作内容（6h） |
|---|--------------|
| D15 | 设计 `ISourceNode<void, ByteChunk>` 接口；约定 ByteChunk 大小（默认 64KB）、超时语义、EOF 标志 |
| D16 | 实现 `FileSourceNode`：分块读 + 进度回报 + Seek 跳转字节偏移；写单测 |
| D17 | 重构 `FfmpegDemuxerNode`：输入改为 `ByteChunk` Link，通过 AVIOContext 自定义 read 回调把 ByteChunk 喂给 libavformat |
| D18 | Pipeline::Build 接入新链路：FileSource → FfmpegDemuxer → ...；本地文件联调 |
| D19 | 修 Seek：FileSource 跳字节 + Demuxer Flush 协同；4K 大文件 seek 验证 |

### 关键决策
- **AVIOContext 自定义读回调**：让 libavformat 误以为是普通文件，但实际从我们的 ByteChunk 队列拿字节。这是兼容 libavformat 与自定义 source 的标准做法
- **Seek 协议**：Source 节点接收字节偏移，Demuxer 节点接收时间戳；上层 Pipeline 统一对外暴露时间戳 Seek

---
## 阶段 3：自写 FLV Demuxer（D20–D31）

**目标**：完全脱离 libavformat 处理 FLV 容器，纯 C++ 位操作解析二进制字节流。下游解码仍走 libavcodec。

**里程碑**：能讲清 FLV 协议每一个字节、能播完整 flv 文件、能讲 I/P/B 帧标记和 CompositionTime 重排原理。

| 天 | 工作内容（6h） |
|---|--------------|
| D20 | 调研 FLV 规范 v10.1：Header（9 字节）+ Tag 结构图，画在笔记里 |
| D21 | 实现 `BitReader` 工具类：按 bit/byte 读，UI24/UI32_BE，字节对齐校验 |
| D22 | 实现 FLV Header + PreviousTagSize 解析；判断有无 audio / video flag |
| D23 | 实现 Script Tag (AMF0) 解析：取 width / height / duration / framerate；写单测 |
| D24 | 实现 Audio Tag 解析：SoundFormat=10(AAC)，第 1 字节 AACPacketType=0 时为 AudioSpecificConfig（喂 extradata），=1 时为 raw AAC |
| D25 | 实现 Video Tag 解析：CodecID=7(H.264)，AVCPacketType=0 为 AVCDecoderConfigurationRecord（取 SPS/PPS），=1 为 NALU |
| D26 | NALU 长度前缀（4 字节）转 Annex-B 起始码 (0x00000001) 的转换工具；或保留长度前缀给 avcc 模式解码 |
| D27 | 处理 CompositeTime：DTS = Tag Timestamp，PTS = DTS + CompositionTime（B 帧重排序） |
| D28 | 封装 `FlvDemuxerNode`：输入 ByteChunk，输出 Packet 双路；流式状态机处理跨 chunk 边界 |
| D29 | 测试用例：B 站下载的 flv / OBS 推流 flv 各 3 个，对比 ffprobe 输出 PTS 完全一致 |
| D30 | Pipeline 注册：根据 url 扩展名自动选 FLV / Ffmpeg demuxer；UI 不感知 |
| D31 | 修边界 bug：截断包、损坏 tag、首帧不是关键帧等场景；写文档 |

### 关键决策
- **不追求功能完备**：目标是覆盖测试集 + 能讲清协议结构，不需要和 ffmpeg 同等鲁棒
- **NALU 格式转换**：libavcodec 既能吃 Annex-B 也能吃 AVCC，配置 `extradata` 时让两边语义一致即可
- **跨 chunk 边界**：FlvDemuxerNode 内部维护字节累积 buffer + 解析状态机，输入数据不够时缓存等待

---
## 阶段 4：硬件解码 + 零拷贝渲染（D32–D47）

**目标**：打通 GPU 数据流，让硬解出来的显存帧直接被 OpenGL 当纹理用，砍掉显存→内存的拷贝开销。

**里程碑**：4K@60 视频软解 CPU ≥80% → 硬解零拷贝 CPU ≤20%，画面不退化。

| 天 | 工作内容（6h） |
|---|--------------|
| D32 | 调研当前平台：Linux→VAAPI / VDPAU、macOS→VideoToolbox、Windows→DXVA2 / D3D11VA；确定先做哪一个（建议 Linux/VAAPI） |
| D33 | 实现 `HwDeviceContext` 封装：`av_hwdevice_ctx_create` + 释放；写探测函数 |
| D34 | `VideoDecoderNode` 加 `EnableHardwareAccel(type)`：配置 `hw_device_ctx`、`get_format` 回调返回硬件 pix_fmt |
| D35 | 让 `VideoFrame` Hardware variant 真正能持 `AVBufferRef*`（FFmpeg 帧引用计数）+ `hw_handle_` 派生 |
| D36 | 不做零拷贝先跑通：硬解出帧后调 `av_hwframe_transfer_data` 降级到 Software variant，验证渲染链通 |
| D37 | 开始零拷贝（以 Linux/VAAPI 为例）：从 VASurfaceID 导出 DMA-BUF fd（`vaExportSurfaceHandle`） |
| D38 | OpenGLView 实现 `HwTextureImporter`：`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` + `glEGLImageTargetTexture2DOES` |
| D39 | NV12 双平面 Shader：Y 用一张纹理，UV 用一张 RG 纹理；测试 1080p 流畅播放 |
| D40 | 验证零拷贝路径：top/perf 观察 CPU 占用应该 < 软件路径 1/3；可视化每帧路径标志位 |
| D41 | 录制兼容性：若开启录制，强制软解（最简单方案）；写 fallback 日志 |
| D42 | 4K HEVC 测试视频压测，记录 CPU/GPU 占用对比表 |
| D43 | 处理硬解失败 fallback：`hwaccel` 不支持时自动降级为 sw，不能崩 |
| D44 | macOS VideoToolbox 路径：CVPixelBufferRef + `CVOpenGLTextureCacheCreateTextureFromImage`（可选） |
| D45 | Windows D3D11VA 路径：`wglDXRegisterObjectNV` + `IDXGIKeyedMutex`（可选，跨平台只做一个先） |
| D46 | 编译开关 `WITH_HWACCEL`，CMake 探测 EGL / VAAPI / VT / D3D11 |
| D47 | 端到端回归 + 文档：画零拷贝数据流图，记录每个平台的具体 API |

### 关键决策
- **平台先做一个，做透**：阶段目标是 Linux/VAAPI 完整可用；其他平台 1~2 天补齐就够
- **录制场景默认走软解**：硬解 + 录制需要"显存帧给编码器"，工程复杂度高，不在本阶段做
- **Hardware variant 必须能 fallback**：探测失败时主动降级，不能崩

---
## 阶段 5：码率控制参数化（D48–D52）

**目标**：编码节点支持 CBR / CRF / CQP 三种模式可切换，并能在 UI 选择。

**里程碑**：能讲清三种模式的本质区别和适用场景（直播 / 点播 / 压制）。

| 天 | 工作内容（6h） |
|---|--------------|
| D48 | 设计 `RateControlConfig { Mode mode; int target_bitrate; int crf; int cqp; ... }` |
| D49 | `VideoEncoderNode::Init` 接受 RateControlConfig：CBR 设 `bit_rate` + `rc_max_rate` + `rc_buffer_size`；CRF 设 `crf` 私有 opt；CQP 设 `qmin=qmax` |
| D50 | UI 加录制配置面板：模式下拉 + 数值输入；保存到 `FileWriterParameters` |
| D51 | 用 ffprobe 验证三种模式输出文件：CBR 看码率波动应窄，CRF 看 QP 自适应，CQP 看 QP 恒定 |
| D52 | 写笔记：直播为什么 CBR、点播为什么 CRF、CQP 适用场景；准备面试话术 |

### 关键决策
- **CBR / CRF / CQP 的本质**：CBR = 固定码率，画质自适应；CRF = 固定画质，码率自适应；CQP = 固定 QP，画质和码率都波动
- **直播必须 CBR**：带宽限制，防止突发流量冲垮网络
- **点播 / 压制用 CRF**：在保证画质前提下最省空间

---
## 阶段 6：色彩空间矩阵 + HDR Tone Mapping（D53–D58）

**目标**：现代图形渲染管线进阶。Shader 动态切换 BT.601 / BT.709 / BT.2020 矩阵，支持 HDR10 / HLG 视频的 Tone Mapping。

**里程碑**：HDR10 / HLG / 老 BT.601 视频颜色都正确。

| 天 | 工作内容（6h） |
|---|--------------|
| D53 | OpenGLView Shader 重构：把 BT.601 矩阵改为 uniform 参数，从 VideoFrame.color_space 注入 |
| D54 | 加 BT.709 / BT.2020 矩阵常量；解码端读 `frame->colorspace` 透传到 VideoFrame |
| D55 | 实现 Limited Range / Full Range 偏移区分：从 `color_range` 控制 16/235 vs 0/255 |
| D56 | HDR 检测：`AVCOL_TRC_SMPTE2084(PQ)` 或 `AVCOL_TRC_ARIB_STD_B67(HLG)` 时进入 HDR 分支 |
| D57 | Shader 实现 PQ EOTF 反变换 + Reinhard tone mapping：`L' = L / (1 + L)`，输出到 SDR |
| D58 | 测试视频：HDR10 mp4 / HLG hevc / 普通 BT.601 老视频 各播一遍，对比颜色 |

### 关键决策
- **色彩转换全在 Shader 里做**：CPU 软算法浪费，多一个节点就多一次内存拷贝；mpv / Chromium 都是 Shader 路线
- **Tone Mapping 选 Reinhard**：算法简单、Shader 友好；上限是 Hable / ACES，本阶段不做

---
## 阶段 7：自写 MP4 Demuxer + HTTP Source（D59–D72）

**目标**：完全脱离 libavformat 解析 MP4 容器；接入 HTTP 拉流；理解 moov 位置对秒开的影响和 fmp4 结构。

**里程碑**：能播在线 mp4 + fmp4；能讲清"为什么 moov 在前面 = 秒开"。

| 天 | 工作内容（6h） |
|---|--------------|
| D59 | 调研 ISO/IEC 14496-12：MP4 box（atom）树结构 ftyp / moov / mdat / moof |
| D60 | 实现 `BoxReader`：4 字节 size + 4 字节 type 头，递归解析 |
| D61 | 解析 moov→trak→mdia→minf→stbl 路径，把 stsd（SampleDescription）拿到 |
| D62 | 解析 stts / stsc / stsz / stco / co64 这四张表，构造 sample 索引表（每个 sample 的文件 offset + size + dts） |
| D63 | 对 H.264：从 stsd 的 avcC box 提取 SPS/PPS；对 AAC：从 esds 提取 AudioSpecificConfig |
| D64 | 实现 `Mp4DemuxerNode`：按 sample 索引 seek 到 mdat 内 offset，读 sample bytes，封 Packet |
| D65 | CTS 偏移：从 ctts box 算 PTS = DTS + CompositionTimeOffset，处理 B 帧顺序 |
| D66 | 测试：本地 mp4 各种来源（H.264/HEVC，AAC/MP3，含 B 帧），对比 ffprobe |
| D67 | fmp4 支持：识别 moof + mdat 切片；解析 tfhd / trun box；记录笔记"为什么 fmp4 是 DASH / HLS 基础" |
| D68 | 实现 `HttpSourceNode`：用 libcurl streaming GET，支持 Range 请求 |
| D69 | HttpSource + Mp4Demuxer 联调：在线 mp4 播放，但要支持渐进式（先下到 moov 再开播） |
| D70 | "为什么 moov 在前面 = 秒开"：写一个小工具检测 moov 位置，对比首帧延迟 |
| D71 | Range Seek：Demuxer 根据 sample offset 反推 HTTP byte range，HttpSource 重发请求 |
| D72 | 性能：1080p 在线 mp4 起播延迟 < 2s；写阶段文档 |

### 关键决策
- **moov 必须在 mdat 之前**：否则要把整个文件下载完才能解析索引表，秒开变成"等开"
- **fmp4 是 DASH/HLS 基础**：每个分片自带 moof，可以独立索引，天然适合 ABR 自适应码率切换
- **Range 请求依赖服务器支持**：libcurl 加 `CURLOPT_RANGE`，服务器返回 206 Partial Content

---
## 阶段 8（备选）：WebRTC / QoS 调研笔记

**目标**：不实现代码，只写一份技术调研文档，回答面试核心问题。预计 3~5 天阅读 + 出文档，**不阻塞主进度**。

### 调研主题清单

- **Jitter Buffer 算法**：固定大小 vs 自适应（Adaptive Jitter Buffer）；何时该 stretch / shrink 音频
- **FEC（前向纠错）**：XOR 异或最简单实现，UlpFEC、FlexFEC 的区别；用带宽换延迟
- **ARQ（自动重传）**：NACK 重传，最大重传次数、RTT 估算；用延迟换带宽
- **FEC vs ARQ**：低延迟实时音视频用 FEC，可容忍延迟的场景用 ARQ
- **拥塞控制**：GCC（基于延迟梯度） vs BBR（基于带宽探测）的基本原理
- **WebRTC 完整数据流**：ICE → DTLS → SRTP → RTP → Depacketizer → Decoder
- **现代流媒体传输协议对比**：RTMP / HLS / DASH / WebRTC 的延迟与穿透性

### 产出
- `docs/webrtc-research.md` 一份完整的调研笔记
- 配套架构图：QoS 完整管线（用阶段 1 的 DAG 词汇画）
- 在简历加分项里清晰列出"了解 WebRTC 整体架构、能讲清各层职责"

---
## 执行原则

以下原则适用于每一个阶段，**比单阶段计划更重要**：

1. **每个阶段独立 git 分支**，例如 `refactor/dag`、`feat/flv-demuxer`、`feat/hw-decode`…每阶段合并到 main 前必须冒烟测试通过
2. **阶段 1 是地基，宁可慢不可省**：地基歪了后面 7 个阶段全要回头改。如果第 14 天没跑通回归，宁可延期 3~5 天也不要带病合并
3. **自写 Demuxer 不追求功能完备**（阶段 3 / 7）：覆盖测试集 + 能讲清协议结构即可，不需要和 ffmpeg 同等鲁棒
4. **每阶段产出一份 markdown 笔记**：协议字段图、踩坑记录、性能数据。这些笔记可直接转化为面试材料
5. **跨平台先做一个**：阶段 4 的硬解先把 Linux/VAAPI 做透，其他平台 1~2 天补齐即可
6. **不追求"一次到位的优雅"**：阶段 1 跑通后，旧文件可能还有些"过渡代码"，留到对应阶段做相关功能时再清理
7. **每阶段结束做一次 1 小时 demo 自我回顾**：能不能用 5 分钟讲清这阶段做了什么、踩了什么坑、性能数据如何

---

## 进度看板（更新此表跟踪进度）

| 阶段 | 状态 | 开始日期 | 完成日期 | 备注 |
|-----|------|---------|---------|-----|
| 阶段 1：DAG 框架与模块迁移 | 未开始 | – | – | – |
| 阶段 2：Source 节点抽象 | 未开始 | – | – | – |
| 阶段 3：自写 FLV Demuxer | 未开始 | – | – | – |
| 阶段 4：硬件解码 + 零拷贝 | 未开始 | – | – | – |
| 阶段 5：码率控制参数化 | 未开始 | – | – | – |
| 阶段 6：色彩空间 + HDR | 未开始 | – | – | – |
| 阶段 7：自写 MP4 + HTTP | 未开始 | – | – | – |
| 阶段 8：WebRTC 调研 | 未开始 | – | – | – |
