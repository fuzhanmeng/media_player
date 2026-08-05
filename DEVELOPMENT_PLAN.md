# 开发计划书 - 里程碑驱动的播放器重构路线

> 目标：从零设计可验证的强类型 DAG，依次完成本地软件播放、音频与同步、Seek/EOF/错误闭环、录制、VA-API 与零拷贝、色彩管理、HTTP 网络播放、RTP 接收与 Jitter Buffer，并最终建立可复现的弱网验证。

## 计划口径

- `Dxx` 是稳定的交付编号，不再承诺“一天完成一个编号”。一个里程碑只有满足验收条件才能进入下一项。
- 正式完成的依据只能来自 `mediacore/`、`app/` 的正式构建和测试；`_review_temp/` 只用于设计评审，不能作为完成证明。
- 当前优先级是完成 D9。D10 保持已完成，D11 尚未开发。
- 每个阶段先建立最小闭环，再增加性能和高级功能；不让硬解、录制或网络反向污染尚未稳定的本地软解主链。
- 项目的产品边界是“播放器”。新能力必须进入真实播放流程，并能用固定样本或可复现环境验证；不为覆盖技术名词单独增加模块。

从当前 D9 开始，唯一产品主线如下，后一阶段以前一阶段的正式验收为前提。D10 是已经完成的独立 DTO 里程碑，不表示 D9 已经通过验收：

```text
稳定 DAG
  -> 本地软件播放器
  -> 音频与同步
  -> Seek/EOF/错误闭环
  -> 录制
  -> VA-API 与零拷贝
  -> 色彩管理
  -> HTTP 网络播放
  -> RTP 接收 + Jitter Buffer
  -> 可复现弱网测试
```

状态标记：`✅ 已完成`、`🔄 进行中`、`⬜ 未开始`、`🔶 部分完成`、`可选`。

## 一、最终分层与核心约束

```text
Qt UI
  -> IPlayer / PlayerFacade       用户命令、PlaybackState、错误事件
      -> Pipeline                 媒体编排、DAG 所有权、启停、暂停、Join
          -> INodeBase/INode      单节点线程循环和业务 Tick
          -> Link<T>              强类型有界数据边
          -> SyncContext          运行门控和 epoch
```

### 状态域

| 状态域 | 所属层 | 内容 | 谁可以修改 |
|---|---|---|---|
| `PlaybackState` | `PlayerFacade` | Idle/Opening/Playing/Paused/Stopped/Error | Player 控制线程 |
| `PipelineState` | `Pipeline` | D9 使用 Building/Starting/Running/Stopping/Stopped；D15 再增加媒体会话阶段 | Pipeline 控制线程 |
| `ExecutionState + epoch` | `SyncContext` | Running/Paused + Seek 世代 | Pipeline；Node 只读 |
| `NodeState` | `INodeBase` | Created/Running/StopRequested/Finished/Joined | Pipeline 和 Node 工作线程 |

状态名称可以继续精简，但不能把 `PlaybackState`、`PipelineState`、`ExecutionState` 和
`NodeState` 合并成一个“全局状态”；它们的所有者、读写线程和生命周期都不同。

### 所有权和生命周期硬约束

1. `PlayerFacade` 同一时刻独占一个 `Pipeline`；这不是进程级单例，多播放器实例可以各有一条 Pipeline。
2. D9 先实现 `Pipeline` 的 DAG 生命周期部分；D15 在同一个类上增加 Open/Build，D20 再增加 Seek，不引入第二个图级所有者。
3. Pipeline 创建并登记全部 Node 和 Link。Node 不创建 Link，不知道完整拓扑，也不负责遍历或 Abort Link。
4. Node 析构函数不执行 Stop/Join。Pipeline 必须先停止并 Join 全部 Node，之后才能销毁 DAG。
5. `Start` 按 Sink 到 Source 的逆拓扑顺序执行，保证消费者先启动。
6. 立即停止固定为：SignalStop 全部 Node -> Abort 全部 Link -> Interrupt 外部 IO -> 唤醒暂停线程 -> Join 全部 Node。
7. `Node::CloseOutput` 通过 `Link::Close` 传递正常 EOF 并允许下游排空；`Abort` 表示立即停止并丢弃存量。录制使用独立的 Drain 协议，不能复用立即 Abort。
8. 控制 API 由一个控制线程串行调用。atomic 只用于工作线程确实需要并发读取的状态，不能用 atomic 掩盖复合状态跃迁竞态。
9. 一条 Link 默认是一条单生产者、单消费者数据边。广播必须由 Tee 创建多条 Link，不能让两个消费者竞争同一队列。
10. `Open`、网络读取、音频写入等外部阻塞调用必须接受独立的 cancellation/deadline。控制命令可以串行，但 Stop 不能排在一个无限阻塞的 Open 后面等待。

## 二、legacy 问题边界与替换原则

legacy 只用于说明旧项目覆盖过的业务场景和已经暴露的架构问题，不是新代码的设计依据、实现模板或迁移来源。新 DAG 必须根据当前目标独立设计、独立实现、独立验证；不得复制 legacy 的类层次、线程模型、回调链、状态共享方式或生命周期处理。

| legacy 暴露的问题 | 新架构必须满足的约束 |
|---|---|
| `Player`/`FileReader` 集中路由数据并手工启停全部组件 | `PlayerFacade` 只处理用户命令；`Pipeline` 统一负责媒体编排和 DAG 生命周期 |
| Demuxer/Decoder/AVSync 各自创建线程、队列并在析构中兜底 Stop | Node 不拥有图生命周期；Node 必须由 Pipeline 停止并 Join 后才能析构 |
| 多层裸 Listener 回调形成控制流反转和跨线程生命周期风险 | 媒体数据只通过强类型 `Link<T>` 单向流动；状态与错误通过明确的控制接口上报 |
| Pause、Seek、Stop 依赖多组共享标志和跨模块握手 | Pause 使用统一等待协议；Seek 使用 epoch + 有序 Flush；Stop 使用固定分阶段协议 |
| Queue 的 Clear/Abort/Start 可以复位并混合正常结束与强制退出 | Link/Queue 一次性使用，严格区分正常 Close、立即 Abort 和录制 Drain |
| FFmpeg 读取错误可能被当作 EOF，阻塞 IO 缺少完整取消 | EOF、Error、Stop 使用不同结果；所有外部阻塞调用必须支持 cancellation/deadline |
| 录制与主播放通过回调耦合，慢录制可能影响播放 | 录制分支具有独立背压策略和生命周期，禁止修改运行中主 DAG 的拓扑 |

VA-API、DMA-BUF/EGL、RTP 和 Jitter Buffer 都是新项目新增能力，必须从契约、测试夹具和能力探测开始，不能从 legacy 推导其设计。

## 三、开发里程碑

### 阶段 A：现有基础与 DAG 核心（D1-D10）

| ID | 状态 | 交付目标 | 验收出口 |
|---|---|---|---|
| D1-D2 | ✅ | CMake/Conan/Qt 外壳，`IPlayer`/`IVideoSink` 公共契约 | 正式应用可构建启动 |
| D3 | 🔶 | Controller 与 PlayerWidget 控制链 | 播放/暂停已接通；进度回填仍待真实 Player |
| D4-D5 | ✅ | OpenGL 黑框与 `StubPlayer` 测试帧 | UI -> IPlayer -> IVideoSink -> OpenGL 闭环 |
| D6 | ✅ | 有界 `ThreadSafeQueue<T>` 原型 | 双 CV、背压、Close/Abort 基本语义 |
| D7 | ✅ | 强类型 `Link<T>` 原型 | Send/Receive/Close/Abort |
| D8 | ✅ | `INodeBase`/`INode<InT,OutT>` 原型 | 节点类型层次已建立 |
| D9 | 🔄 | DAG 生命周期重构和正式并发测试 | 见下方 D9 完成标准 |
| D10 | ✅ | `Packet` RAII、微秒时间戳、flags、epoch 字段 | 保持现状，不夹带 D9 生命周期职责 |

### D9 当前唯一主线：Pipeline 的 DAG 生命周期核心

按以下依赖顺序开发，不先写真实媒体 Node：

1. 冻结 Queue 的 `Push/Pop/Close/Abort` 一次性语义，容量 0 必须拒绝。
2. 冻结 Link 的数据接口、类型擦除控制接口和创建权限；Link 只能经 Pipeline 创建并登记。
3. 重写 `SyncContext`，保证 Pause/Resume/Stop 的状态写入、等待注册和通知遵循同一 mutex/CV 协议。
4. 重写 `INodeBase`：一次性 Start、固定 ThreadMain、`TickResult`、异常边界、SignalStop、InterruptBlockingIo、Join。
5. `INode<InT,OutT>` 只提供零/单输入输出端口便利；多输入/多输出节点直接持有所需端口，不引入 `Link<void>`。
6. 实现 `Pipeline::CreateLink/EmplaceNode/Start/Pause/Resume/Stop`。D9 不实现媒体探测、Seek 或 Player FSM。
7. 使用假 Source/Transform/Sink 建正式 CTest，不使用真实 FFmpeg 节点证明并发正确性。

D9 必须同时通过：正常 Close 排空、空队列 Receive 停止、满队列 Send 停止、暂停中停止、反复 Pause/Resume/Pause、外部 IO 模拟中断、部分启动失败回滚、重复 Stop、Tick 异常、Node 先 Join 后析构、拓扑启动顺序。测试进入正式 CMake，并运行普通构建、ASan/UBSan 和 TSan。

### 阶段 B：本地视频最小闭环（D11-D15）

| ID | 交付目标 | 关键内容 | 验收出口 |
|---|---|---|---|
| D11 | StreamInfo + Demuxer | 先支持本地文件路径；接口保留 FFmpeg URL 输入能力；可取消的同步 Open 探测、读包双输出、interrupt callback、EOF/Error/Stop 分离 | 用固定本地媒体样本验证 Packet 数量、时间戳、双输出、Open/Read 超时和阻塞停止；此阶段不声明 HTTP 已支持 |
| D12 | Frame 与控制协议 | `VideoFrame` Software 所有权、linesize、色彩元数据；`AudioFrame`；kFlush/kEOF；结构化错误 | DTO 单测、RAII 和 move/copy 规则明确 |
| D13 | 软件视频解码 | send/receive、EAGAIN、B 帧 drain、best_effort_timestamp、像素格式转换 fallback | Packet -> VideoFrame 测试，帧数/PTS/分辨率可验证 |
| D14 | 软件视频输出 | `VideoSinkNode`、GUI 线程投递、YUV 三平面上传、基础 BT.601/709 limited/full | 第一帧和连续帧正确显示，无跨线程 GL 调用 |
| D15 | 最小媒体 Pipeline | 在 D9 Pipeline 上增加 `Open/Build`；接入 `PlayerFacade + FSM`；串行控制执行器与可并发触发的 IO 取消；视频-only 自动组图 | 本地视频 Open/Play/Pause/Resume/Stop/Reopen 端到端通过 |

D15 是第一个真实垂直里程碑。完整 Pipeline 在这里落地，而不是等所有音频、同步、硬解节点写完后一次性集成。

### 阶段 C：音频、同步、Seek 与错误闭环（D16-D20）

| ID | 交付目标 | 关键内容 | 验收出口 |
|---|---|---|---|
| D16 | AudioDecoderNode | avcodec + swr，统一输出格式，正确处理可变 nb_samples 和 drain | PCM 样本数、时长和 PTS 可验证 |
| D17 | AudioSinkNode | `IAudioSink`、ALSA 参数协商、可中断写、xrun 恢复、`snd_pcm_delay` 音频时钟 | 音频-only 播放、暂停、欠载和停止通过 |
| D18 | AVSyncNode | 注入 `IClockSource`；音频主时钟；视频-only 墙钟 fallback；可中断 wait_until | A/V 样本连续播放，同步偏差有指标而非目测 |
| D19 | 媒体控制闭环 | Pause/Resume 对 ALSA、时钟和各节点的语义；正常 EOF 汇聚；播放完成事件 | 高频暂停恢复、EOF、音频-only、视频-only、A/V 文件回归 |
| D20 | Seek 与错误闭环 | epoch 过滤、带内 Flush、decoder/audio device flush、last-writer-wins Seek、Open/Read/Decode 错误上报 | 播放态/暂停态/EOF 后 Seek，无旧帧/旧音频泄漏；Stop/Reopen 稳定 |

D20 完成后才称为“本地软件播放器主链完成”。

### 阶段 D：录制分支与优雅排空（D21-D23）

| ID | 交付目标 | 关键内容 | 验收出口 |
|---|---|---|---|
| D21 | Tee 与分支策略 | 一帧多 Link；主播放阻塞背压、录制分支可配置丢帧；在“构建时固定休眠分支”和“独立录制 Pipeline”中定型一种方案，禁止修改运行中的主 Pipeline 拓扑 | 反复开始/停止录制不改主图，慢录制不能拖停主播放，丢帧有计数 |
| D22 | 编码节点 | VideoEncoderNode、AudioEncoderNode、输入 PTS 归一化、编码器 drain | H.264/AAC Packet 时间戳单调且可复现 |
| D23 | MP4 Muxer 与 Drain | 双输入交错、单轨兼容、写 trailer；`DrainAndStop` 与立即 Stop 分离 | 输出文件可用 ffprobe 校验并可 Seek；异常退出有明确损失边界 |

硬解开启后的录制策略必须显式选择：硬件帧下载到软件编码器、增加硬件编码器，或暂时禁用录制；不能让调用方隐式踩中格式不兼容。

### 阶段 E：VA-API、零拷贝与色彩（D24-D31）

| ID | 交付目标 | 关键内容 | 验收出口 |
|---|---|---|---|
| D24 | 硬件能力门禁 | `vainfo`、DRM render node、EGL/GL 扩展、Qt 上下文线程和共享可行性；建立支持矩阵 | 不支持环境明确跳过，支持环境保存探测报告 |
| D25 | 硬件帧契约 | `HwDeviceContext`；Hardware VideoFrame 的 AVFrame/AVBufferRef/FD/descriptor RAII；色彩元数据 | 所有权和释放线程规则有单测/断言 |
| D26 | VA-API 解码正确性 | `av_hwdevice_ctx_create`、`get_format`、VAAPI frame；先 `av_hwframe_transfer_data` 回软件路径 | 硬解得到与软解一致的可见画面，失败可回退 |
| D27 | DMA-BUF 导出 | `vaExportSurfaceHandle`、多对象/多平面、offset/stride/modifier、FD 所有权 | 描述符验证，无 FD 泄漏和提前释放 |
| D28 | EGLImage 导入 | GUI/渲染线程创建销毁 EGLImage，NV12/P010 纹理导入，surface lifetime fence | 连续播放、Stop/Reopen 和窗口销毁无黑帧/悬垂资源 |
| D29 | 双路径集成与性能 | software/hardware 自动选择、运行时 fallback、1080p/4K CPU/GPU/内存/掉帧基准 | 数据证明收益；fallback 不崩溃、不泄漏 |
| D30 | SDR 色彩管理 | BT.601/709/2020、limited/full range、矩阵与采样格式组合 | 与参考图/色条做像素级或容差比较 |
| D31 | HDR 到 SDR | PQ/HLG 识别、EOTF、tone mapping、输出显示假设 | HDR 样本无明显 clipping，参数和参考结果记录完整 |

零拷贝以“无 GPU -> CPU -> GPU 像素拷贝”为目标，不以“代码中没有 memcpy”作为证明。必须同时检查 `av_hwframe_transfer_data` 调用、DMA-BUF FD、CPU 占用和 GPU 引擎数据。

### 阶段 F：HTTP 网络播放、RTP 接收与弱网验证（D32-D39）

网络阶段先完成 HTTP 播放，再实现 RTP 接收与 Jitter Buffer，最后用可复现的弱网环境验证播放行为。HTTP 和 HLS 首先使用 FFmpeg 已有的 URL/协议能力，不预设自研网络协议层。D39 是端到端弱网环境与报告的收口，不表示 D32-D38 可以推迟各自的单元测试和集成测试。

| ID | 交付目标 | 关键内容 | 验收出口 |
|---|---|---|---|
| D32 | HTTP 播放基线 | `FfmpegDemuxerNode(url)` 直接使用 FFmpeg HTTP；Open/Read 超时、interrupt callback 和 Stop 取消 | 本地 HTTP 服务下可连续播放，连接或读取阻塞时可在限时内停止 |
| D33 | HTTP 缓冲与恢复 | 缓冲中/恢复状态，有界队列和背压，只对可恢复错误做有限次重连；重连期间仍可 Stop | 限速、暂断和慢读下的状态迁移、队列上限、重连次数和退出时间可验证 |
| D34 | FFmpeg 能力验收与自定义 HTTP 决策门 | 验证 Range/Seek、重定向和 FFmpeg 支持的 HLS 样本；只有 FFmpeg URL 无法满足缓存观测或取消需求时，才评估 `HttpSourceNode + AVIOContext` | 形成可复现的能力表和决策记录；自定义 HTTP 层不是默认验收项 |
| D35 | RTP 契约与解析 | UDP 取消、RTP header、SSRC/PT/seq/timestamp wrap、H.264 FU-A/STAP-A；录制包 fixture | parser/depacketizer 纯单测覆盖截断、乱序、重复、wrap |
| D36 | 自研固定 Jitter Buffer | RTP 包按扩展 seq 重排，再按 deadline 最终判丢；明确固定窗口、重复包、迟到包、空洞和容量上限策略 | 使用确定性虚拟时钟和固定乱序 trace 的正式测试验证，不依赖 sleep |
| D37 | RTP 视频闭环 | `RtpSource -> JitterBuffer -> H264Depacketizer -> VideoDecoder`；SPS/PPS/IDR 恢复 | H.264 RTP 视频可播，丢 FU 分片时丢弃完整损坏 AU |
| D38 | Jitter 自适应与播放恢复 | 基于已测 jitter 和迟到包比例，在明确上下限内调整目标延迟；处理断流、重连、SSRC 切换、Jitter Buffer 清理、SPS/PPS 恢复和关键帧等待 | 固定 trace 下的延迟/迟到丢包取舍可对比；不同恢复路径均有明确状态、超时和首个可解码帧验收 |
| D39 | 可复现弱网测试 | 在 HTTP 和 RTP/Jitter 闭环完成后，建立固定延迟、带宽受限、丢包、乱序、重复、抖动和断流 profile；定义首帧、稳定延迟、rebuffer、队列深度和恢复时间 | 使用固定 trace/随机种子与可脚本化网络环境，为 HTTP 和 RTP 产生可重复、可比较的报告；Jitter Buffer 不通过无限增大延迟换取表面平稳 |

默认 RTP 播放数据顺序是：

```text
UDP/RTP
  -> Jitter Buffer 重排与最终判丢
  -> H.264 Depacketizer / Access Unit
  -> VideoDecoder
```

RTP 第一版只承诺 H.264 视频。Jitter Buffer 由本项目自行实现，并以确定性单测、录制包回放和 D39 弱网测试作为正式验收。客户端冷启动在收到首个可解码 IDR 前无法生成可播放 GOP，因此首帧时间只能在测试源 IDR 周期、网络 RTT 和统计口径明确后作为指标，不能写成无条件承诺。

FEC 不在默认 RTP 数据流中，也不是 D35-D40 的完成条件。只有在发送端行为、线上包格式、丢包恢复契约和固定样本全部明确后，才单独评估是否将 FEC 接入 Jitter Buffer 之前。没有可互操作的对端和可验证收益时，不实现 FEC。

### 阶段 G：系统收口（D40）

D40 不增加第十一项产品能力，只在 D39 之后对已完成能力做全量回归、性能记录和文档收口。

1. 建立本地/HTTP/RTP、音频-only/视频-only/A-V、软解/硬解、播放/暂停/Seek/Stop/Reopen、录制/非录制的回归矩阵。
2. 固化性能表：首帧时间、稳定延迟、同步偏差、rebuffer ratio、CPU/GPU、内存、队列峰值和 FD 数量。
3. 同步更新 README、ARCHITECTURE 和源码树状态，删除所有无法由正式构建与测试复现的“已实现”描述。

## 四、每个里程碑的完成定义

一个里程碑必须同时满足以下条件才能标记 `✅`：

1. 代码位于正式目录并显式加入 CMake target；头文件 PUBLIC/PRIVATE 边界正确。
2. 使用固定输入和确定性条件验证，不用 `sleep` 碰运气证明并发正确。
3. 失败、EOF、Stop、部分初始化失败和重复调用路径都有测试。
4. 普通构建开启 `-Wall -Wextra -Wpedantic`；新增警告必须修复。
5. 所有权/并发改动通过 ASan/UBSan；线程协议改动通过 TSan。环境不支持某项 Sanitizer 时必须记录原因，不能写成已通过。
6. 集成测试有超时，死锁以测试失败结束而不是永久挂起。
7. 日志和错误包含稳定模块名、错误码及 FFmpeg/系统错误文本；不把错误伪装成 EOF。
8. 文档只描述正式仓库当前能力，目标架构和当前状态明确分栏。

## 五、执行顺序与范围取舍

- 从 D9 起按本文的产品主线依次验收；D10 保持已完成，D9 通过后再进入 D11。时间不足时停在已完成的前一阶段，不跳过录制或色彩管理直接堆叠网络功能。
- D9、D15、D20 是 DAG、本地视频和完整软件播放器的三个基础验收点，不压缩其生命周期、错误处理和测试预算。
- RTP 阶段的核心是接收、重排、解包、解码和播放恢复。发送端速率控制、完整实时通信协议栈、语音前处理和传输服务器不在当前播放器主线中。
- HTTP 以 FFmpeg URL 播放为默认实现，HLS 作为 FFmpeg 支持格式进行播放验证。只在可重现的限制已被证明后，才增加自定义 HTTP 数据源。
- FEC 是有前置条件的后续决策项，不是默认功能、主线里程碑或项目收口条件。

## 六、当前进度快照

| 阶段 | 状态 | 当前结论 |
|---|---|---|
| UI 与公共接口（D1-D5） | ✅/🔶 | StubPlayer 渲染闭环可用，进度显示待真实 Player |
| DAG 核心（D6-D9） | 🔄 | Queue/Link/Node 有原型，D9 生命周期重构和正式并发验证未完成 |
| 基础 DTO（D10） | ✅ | Packet 与 flags 保持现状 |
| 第一个真实 Node（D11） | ⬜ | 未开发，必须等待 D9 验收 |
| 本地播放、录制、硬解、色彩、网络 | ⬜ | 均未进入正式实现 |

> 下一步唯一任务：完成 D9 的 Pipeline DAG 生命周期核心、INodeBase、SyncContext、Link 生命周期协议及正式并发测试。D9 验收前不开始 D11。
