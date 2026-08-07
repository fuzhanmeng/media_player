# 开发计划书

> 本文是 ai-media-player 从 Day 01 到项目完成的完整开发计划。
>
> 只回答"要做什么、按什么顺序做、做到什么程度算完成"，不记录实际进度。
> 仓库实现到哪一步，以源码和测试为准。
>
> 只有开发顺序、阶段目标、项目范围或验收标准本身变化时，才修改本文。

---

## 一、计划口径

- 一个 Day 按 6–8 小时有效开发时间估算，编号只表示执行顺序，不表示自然日。
- 一个 Day 没有达到验收标准，就不进入下一个 Day。宁可拆成两天，不要带着未验证的代码往前走。
- 完成的依据只能来自 `mediacore/`、`app/`、`tests/` 的正式构建和测试。
- 每个阶段先建立最小闭环，再加性能和高级功能。不让硬解、录制或网络反向污染尚未稳定的软解主链。
- 项目边界是"播放器"。新能力必须进入真实播放流程，并能用固定样本或可复现环境验证。

阶段顺序：

```text
工程基础 → DAG 核心 → 基础 DTO
  → 解封装 → 解码 → 视频输出 → 最小 Pipeline
  → 音频 → A/V 同步 → 控制闭环 → Seek/EOF/错误
  → 录制
  → VA-API 与零拷贝 → 色彩管理
  → HTTP → RTP + Jitter Buffer → 弱网验证
  → 系统收口
```

| 阶段 | Day | 交付 |
|---|---|---|
| A | 01–10 | 工程基础与 DAG 核心 |
| B | 11–18 | 本地视频最小闭环 |
| C | 19–26 | 音频、同步与控制闭环 |
| D | 27–30 | 录制分支与优雅排空 |
| E | 31–38 | VA-API、零拷贝与色彩 |
| F | 39–43 | 网络播放与抗弱网 |
| G | 44–45 | 系统收口 |

---

## 二、最终分层与核心约束

```text
Qt UI
  → IPlayer / PlayerFacade     用户命令、PlaybackState、错误事件
     → Pipeline                媒体编排、DAG 所有权、启停、暂停、Join
        → INodeBase/INode      单节点线程循环和业务 Tick
        → Link<T>              强类型有界数据边
        → SyncContext          运行门控和 epoch
```

### 状态域

| 状态域 | 所属层 | 内容 | 谁可以修改 |
|---|---|---|---|
| `PlaybackState` | `PlayerFacade` | Idle/Opening/Playing/Paused/Stopped/Error | Player 控制线程 |
| `PipelineState` | `Pipeline` | Building/Starting/Running/Stopping/Stopped | Pipeline 控制线程 |
| `PipelineRunState` + epoch | `SyncContext` | Running/Paused + Seek 世代 | Pipeline；Node 只读 |
| `NodeState` | `INodeBase` | Created/Running/StopRequested/Finished/Joined | Pipeline 和 Node 工作线程 |

这四个状态的所有者、读写线程和生命周期都不同，不能合并成一个"全局状态"。

### 所有权和生命周期硬约束

1. `PlayerFacade` 同一时刻独占一个 `Pipeline`；不是进程级单例，多播放器实例可各有一条。
2. `Pipeline` 先实现 DAG 生命周期，后续在同一个类上加 Open/Build 和 Seek，不引入第二个图级所有者。
3. Pipeline 创建并登记全部 Node 和 Link。Node 不创建 Link，不知道完整拓扑，不负责 Abort Link。
4. Node 析构函数不执行 Stop/Join。Pipeline 必须先停止并 Join 全部 Node，之后才能销毁 DAG。
5. `Start` 按 Sink 到 Source 的逆拓扑顺序执行，保证消费者先启动。
6. 立即停止固定为：SignalStop 全部 Node → Abort 全部 Link → Interrupt 外部 IO → 唤醒暂停线程 → Join 全部 Node。
7. `CloseOutputs` 通过 `Link::Close` 传递正常 EOF 并允许下游排空；`Abort` 表示立即停止并丢弃存量。录制用独立 Drain 协议，不复用 Abort。
8. 控制 API 由一个控制线程串行调用。atomic 只用于工作线程确实需要并发读取的状态。
9. 一条 Link 默认单生产者、单消费者。广播必须由 Tee 创建多条 Link。
10. Open、网络读取、音频写入等外部阻塞调用必须支持独立的 cancellation/deadline。Stop 不能排在一个无限阻塞的 Open 后面等待。

---

## 三、项目范围

**目标能力：** 本地文件播放、音视频同步、Seek、EOF 与错误闭环、录制成 MP4、VA-API 硬解与零拷贝、SDR/HDR 色彩管理、HTTP 拉流、RTP 接收与 Jitter Buffer、可复现弱网验证。

**非目标：**

- 不自写容器解析器，探测和解封装交给 FFmpeg。
- 不实现 RTSP/RTMP、完整 WebRTC、SRT、QUIC。
- 不实现 RTP 发送侧：Pacer、GCC/BBR、动态码率。
- RTP 第一版只做 H.264 视频接收端，不含 RTCP、SRTP、RTP 音频。
- FEC 和 GOP Cache 不属于默认 RTP 数据流，只在指标证明必要时单独评估。
- FFmpeg HTTP 够用就不加 `libcurl + AVIOContext`。
- 不修改运行中的主 Pipeline 拓扑。
- 不承诺脱离测试条件的性能数字。

---

## 四、legacy 边界

`legacy/` 只用于说明旧项目暴露过的架构问题，不是设计依据、实现模板或迁移来源。

| legacy 的问题 | 新架构的约束 |
|---|---|
| `Player`/`FileReader` 集中路由数据并手工启停组件 | `PlayerFacade` 只处理用户命令，`Pipeline` 统一负责编排和生命周期 |
| Demuxer/Decoder/AVSync 各自建线程、队列并在析构中兜底 Stop | Node 不拥有图生命周期，必须由 Pipeline 停止并 Join 后才能析构 |
| 多层裸 Listener 回调造成控制流反转和跨线程生命周期风险 | 媒体数据只经 `Link<T>` 单向流动，状态与错误走明确的控制接口 |
| Pause/Seek/Stop 依赖多组共享标志和跨模块握手 | Pause 用统一等待协议，Seek 用 epoch + 有序 Flush，Stop 用固定分阶段协议 |
| Queue 的 Clear/Abort/Start 可复位并混合正常结束与强制退出 | Link/Queue 一次性使用，严格区分 Close、Abort 和 Drain |
| FFmpeg 读取错误可能被当作 EOF，阻塞 IO 缺少取消 | EOF、Error、Stop 用不同结果；外部阻塞调用必须支持取消 |
| 录制与主播放通过回调耦合，慢录制影响播放 | 录制分支独立背压和生命周期，禁止改运行中主图拓扑 |

VA-API、DMA-BUF/EGL、RTP、Jitter Buffer 是新增能力，从契约、测试夹具和能力探测开始，不从 legacy 推导。

---

## 五、每个 Day 的通用完成标准

除各 Day 自己的验收条件外，以下对每个 Day 都成立：

1. 代码在正式目录并加入 CMake target，头文件 PUBLIC/PRIVATE 边界正确。
2. 用固定输入和确定性条件验证，不用 `sleep` 证明并发正确。
3. 失败、EOF、Stop、部分初始化失败、重复调用路径都有测试。
4. 构建开 `-Wall -Wextra -Wpedantic`，新增警告必须修。
5. 所有权/并发改动过 ASan/UBSan，线程协议改动过 TSan。环境不支持要记录原因。
6. 集成测试有超时，死锁以测试失败结束而不是永久挂起。
7. 日志和错误含模块名、错误码及 FFmpeg/系统错误文本，不把错误伪装成 EOF。
8. 每个 `.h`/`.cpp` 有 Doxygen 文件注释，类和函数说明职责、线程模型和生命周期。
9. 新增的可调参数必须进 `PlayerConfig` 并在第六节的预算表里有一行，不允许在调用点写魔数。
10. 新增模块必须有自己的模块名日志；新增的缓冲、队列或缓存必须有明确上限，不允许无界增长。

---

## 六、配置与资源预算

### 6.1 配置从哪来

所有可调参数集中在一个 `PlayerConfig` 结构里，按模块分组嵌套（`config.demux.*`、`config.decode.*`、`config.audio.*`、`config.network.*`……）。三层来源，后者覆盖前者：

1. **代码内 `constexpr` 默认值** —— 不给任何配置也能跑，这是唯一的默认值定义处；
2. **配置文件** —— 运行时可选加载，文件不存在就用默认值，不报错；
3. **环境变量 / 命令行** —— 调试和测试用的临时覆盖。

三条规则：

- 任何"可配置"的量必须有具名字段和单一默认值来源，调用点不写魔数；
- 配置在 `Open`/`Build` 之前一次性读取并冻结，运行中不修改 —— 与"不改运行中拓扑"的约束一致；
- 每个 Node 只接收自己需要的那一段配置，不持有全局配置对象。

### 6.2 资源预算基线

播放器必须对内存、队列深度和延迟设上限，否则弱网或慢消费者会把内存吃光。下表是**初始值**，作用是让每条链路从第一天起就是有界的；具体数字在 Day 43 用实测校准，在 Day 45 写回默认值。

| 资源 | 配置项 | 初始值 | 依据 |
|---|---|---|---|
| 视频压缩包队列 | `demux.video_packet_capacity` | 200 | 覆盖一个 GOP 的解码抖动即可；1080p 下约 20 MB 上限 |
| 音频压缩包队列 | `demux.audio_packet_capacity` | 200 | 音频包小，主要防 Demux 空转 |
| 解码后视频帧队列 | `decode.video_frame_capacity` | 3 | 队列深度直接等于额外延迟和 Seek 后要丢的帧数，宁浅勿深；抖动由 AVSync 吸收，不靠深队列 |
| 解码后音频帧队列 | `decode.audio_frame_capacity` | 32 | 约 500 ms PCM，够 ALSA 写入抖动 |
| ALSA 设备缓冲 | `audio.device_buffer_ms` | 200 | 太小易 xrun，太大 Pause/Seek 响应迟钝 |
| 音频总缓冲上限 | `audio.max_buffered_ms` | 500 | 队列 + 设备缓冲的合计硬上限 |
| 同步重锚定超时 | `sync.reanchor_timeout_ms` | 500 | Seek 后等不到音频就用墙钟兜底 |
| 录制分支帧队列 | `record.frame_capacity` | 8 | 满了丢帧并计数，不阻塞主播放 |
| HTTP 打开超时 | `network.open_timeout_ms` | 10000 | |
| HTTP 读超时 | `network.read_timeout_ms` | 10000 | |
| HTTP 重连次数 | `network.max_reconnect` | 3 | 超过判为不可恢复 |
| Jitter Buffer 目标延迟 | `jitter.target_delay_ms` | 100 | Day 43 按实测 jitter 在上下限内自适应 |
| Jitter Buffer 延迟上下限 | `jitter.min_delay_ms` / `max_delay_ms` | 50 / 500 | 上限存在的意义是禁止用无限缓存换表面平稳 |
| Jitter Buffer 容量 | `jitter.packet_capacity` | 512 | 超限丢最旧数据并计数 |

内存总量不写成承诺值。Day 43 和 Day 44 用实测数据填 1080p / 4K、软解 / 硬解、本地 / 网络各组合的常驻内存和峰值，作为回归基线。

---

# 阶段 A：工程基础与 DAG 核心（Day 01–10）

## Day 01 — 工程骨架、公共契约与日志

### Objective
建立可构建可运行的空工程，定下 UI 与内核的唯一边界，并让日志从第一行代码起就可用。

### Tasks
- 顶层 `CMakeLists.txt`（C++17、关闭编译器扩展）、`conanfile.py`、`build.sh` / `clean.sh`、`.clang-format` / `.clang-tidy`。
- 建 `mediacore/` 与 `app/`，`include/` 设 PUBLIC、`src/` 设 PRIVATE。
- 公共头：`iplayer.h`（Open/Play/Pause/Stop/Seek/GetState/Attach/DetachDisplayView）、`playback_state.h`、`ivideo_sink.h`、`player_factory.h`、`av_types/frame_size.h`。
- `logger.h` + `logger.cpp`：spdlog 封装，控制台 sink + 文件 sink 双输出。
- 日志格式固定含五项：时间戳（毫秒精度）、线程 id、模块名、级别、消息；文件 sink 额外带源码文件名、行号和函数名。
- 提供 `LOG_TRACE` / `LOG_DEBUG` / `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` / `LOG_FATAL` 宏，基于 spdlog 的 `SPDLOG_LOGGER_*` 以便自动捕获源码位置并支持编译期级别裁剪。
- `Init()` 幂等，`Shutdown()` 先 flush 再释放；error 及以上级别立即 flush，避免崩溃丢日志。
- `app/main.cpp` 创建 `QApplication` 并弹出空窗口，启动时 `Logger::Init()`、退出前 `Shutdown()`。

### Dependencies
无。

### Acceptance Criteria
- `./build.sh` 从干净仓库一次成功，运行后弹窗并正常退出。
- `app` 能 include 全部公共头；尝试 include `mediacore/src/` 下的头时编译失败。
- 接口只暴露抽象类型，不泄漏 FFmpeg 或 Qt 类型。
- 多线程并发写日志不交错、不崩溃；日志行能定位到线程、模块和源码位置。
- 进程异常退出时 error 级别日志已经落盘。

---

## Day 02 — UI 控制链与 OpenGL 画布

### Objective
把用户操作接到 `IPlayer`，并建立能显示的 GL 窗口。

### Tasks
- `ControllerWidget`：播放/暂停按钮 + 进度条，只发信号，不持业务对象，不 include `mediacore`。
- `PlayerWidget`：持 `IPlayer*`（非拥有），接信号到槽，按 `GetState()` 决定 Play 还是 Pause；理清与 `IPlayer` 的析构顺序。
- `OpenGLView` 继承 `QOpenGLWidget` + `QOpenGLFunctions_3_3_Core` + `IVideoSink`，实现 `initializeGL`/`resizeGL`/`paintGL`。
- 编译链接 YUV→RGB 着色器，建全屏矩形几何（VAO/VBO/EBO），创建 Y/U/V 三个 R8 纹理句柄。

### Dependencies
Day 01。

### Acceptance Criteria
- 点击按钮触发对应的 `IPlayer` 调用，窗口关闭无悬垂指针访问。
- 窗口显示纯色画面无 GL 错误，缩放时 viewport 正确更新。
- GL 资源在上下文有效期内释放。

---

## Day 03 — 渲染闭环

### Objective
用桩实现打通 UI → IPlayer → IVideoSink → OpenGL 全链路。

### Tasks
- `video_frame.h`：一次 `av_malloc` 分配 Y+U+V 连续块，`shared_ptr` + `av_free` 删除器托管，三平面指针指向块内偏移；`IsValid()` 校验宽高、buffer、指针和 linesize。
- `StubPlayer`：后台线程合成随时间平移的渐变 YUV420P 帧，条件变量实现 Play/Pause/Stop 三态，暂停不忙等。
- `OnReceiveVideoFrame` 加锁存入信箱（只留最新帧），发信号以 `Qt::QueuedConnection` 投到 GUI 线程；`paintGL` 取帧上传纹理并绘制。
- `player_factory.cpp` 返回 `StubPlayer`。

### Dependencies
Day 02。

### Acceptance Criteria
- 窗口显示连续变化的渐变画面，暂停后画面静止且 CPU 占用回落。
- 后台线程不执行任何 GL 调用。
- 分辨率变化时纹理重新分配而非越界写入。

---

## Day 04 — 有界阻塞队列与强类型数据边

### Objective
实现 DAG 的底层存储、背压原语和只能由 Pipeline 创建的数据边。

### Tasks
- `thread_safe_queue.h`：一把 mutex 保护队列和两个标志，两个 CV 分别等"未满"和"非空"。
- `Push` 满时阻塞、`Close`/`Abort` 后返回 false；`Pop` 空时阻塞，`aborted_` 立即返回 `nullopt`，`closed_` 排空后返回 `nullopt`。
- `Abort()` 先发布状态并通知，最后才 swap 出队列丢弃存量；构造函数拒绝容量 0；CV 一律用谓词版本，改完数据先解锁再通知。
- `link.h`：`Link<T>` 内嵌队列，提供 `Send`/`Receive`/`Close`/`Depth`/`IsClosed`/`IsAborted`，不提供 `CloseInput()`。
- `ILinkControl` 非模板基类，`Abort()` 为 private 纯虚 + `friend Pipeline`；`LinkCreationToken` 私有默认构造 + `friend Pipeline`，允许拷贝移动以穿过 `make_shared`。
- 两个 `static_assert`：令牌不是 aggregate、不可默认构造。

### Dependencies
Day 01。

### Acceptance Criteria
- 满队列 Push 阻塞、有空位后返回 true；空队列 Pop 阻塞、有数据后返回值。
- Close 后 Push 失败但 Pop 可排空；Abort 后收发立即失败且等待线程全部醒来。
- 容量 0 构造抛 `std::invalid_argument`。
- 在 Pipeline 之外构造 `Link<T>` 编译失败；Node 持有 Link 但无法调用 `Abort()`。
- `Link<int>` 与 `Link<Packet>` 能同时存入 `vector<shared_ptr<ILinkControl>>`。

---

## Day 05 — Node 层次

### Objective
建立与媒体类型无关的节点线程基类和端口模板。

### Tasks
- `inode.h`：`INodeBase` 持 `std::thread`，定义 `NodeState`、`NodeStartResult`、`NodeJoinResult`、`TickResult`、`NodeFinishReason`。
- 固定非虚 `ThreadMain()`：暂停等待 → 查停止 → `Tick()` → 再查停止 → 按结果分支；`catch (...)` 兜在线程入口存入 `exception_ptr`。
- `Start()` 用 CAS 从 `kCreated` 切 `kRunning`，区分"已启动"和"启动前已停止"，线程工厂失败时回滚。
- `SignalStop()` / `InterruptBlockingIo()` / `Join()` 三个动作分离，全部 private + `friend Pipeline`。
- 析构函数只 `assert(!thread_.joinable())`；`CloseOutputs()` 声明为纯虚。
- `NoPort` 占位类型 + `INode<InT,OutT>` 及三个偏特化，提供连接与收发接口和 `final` 的 `CloseOutputs()`；连接接口检查 `CanConfigure()` 并拒绝空指针。

### Dependencies
Day 04。

### Acceptance Criteria
- 四种端口形态都能实例化，不产生 `Link<void>` 的实例化尝试。
- 未实现 `Tick()` 的类型是抽象类。
- Node 无法调用自己的 `Start`/`SignalStop`/`Join`。

---

## Day 06 — 运行门控与 epoch

### Objective
实现一张图内全部 Node 共享的暂停协调与世代号。

### Tasks
- `sync_context.h`：`PipelineRunState` 枚举与 atomic 状态。
- `WaitWhilePaused(should_stop)`：模板接收谓词，先无锁快速检查，再加锁进谓词版 `wait`。
- `SetRunning` / `SetPaused` / `WakeAllPausedThreads` / `BumpEpoch` 全部 private + `friend Pipeline`；`GetState` / `GetEpoch` 公开只读。
- `INodeBase` 增加 `ShouldDropStaleItem()` 与 `GetCurrentEpoch()`。
- Node 停止标志留在各自的 `INodeBase`，不进 `SyncContext`。

### Dependencies
Day 05。

### Acceptance Criteria
- 运行态调用 `WaitWhilePaused` 不加锁直接返回。
- 暂停态调用会阻塞，`SetRunning()` 后全部醒来。
- 停止谓词为真时即使处于暂停态也立即返回。
- Node 无法调用任何写接口。

---

## Day 07 — Pipeline 组图与启动

### Objective
实现 DAG 的所有权、组图接口和逆拓扑启动。

### Tasks
- `pipeline.h`：`PipelineState` 用普通枚举（非 atomic），`PipelineStartResult` 枚举。
- 持 `shared_ptr<SyncContext>`、`vector<unique_ptr<INodeBase>>`、`vector<shared_ptr<ILinkControl>>`、可注入的 `NodeThreadFactory`。
- `CreateLink<T>`：四个 `static_assert`（对象类型、非数组、无 cv、非 `NoPort`）+ 状态检查 + 登记到 `links_`。
- `EmplaceNode<NodeT>`：四个 `static_assert`（继承 `INodeBase`、公开无歧义、非抽象、可用 `(shared_ptr<SyncContext>, Args...)` 构造），自动注入 SyncContext，返回借用引用。
- `Start()` 反向遍历 `nodes_`，任一失败调完整 `Stop()` 回滚；`GetSyncContext()` 返回 const 视图。

### Dependencies
Day 06。

### Acceptance Criteria
- 运行中调用 `CreateLink`/`EmplaceNode` 抛 `std::logic_error`。
- 空图 `Start()` 返回 `kNoNodes` 且状态不变。
- 按 Source→Sink 登记时实际启动顺序为 Sink→Source。
- 同一 Pipeline 只能 Start 一次。

---

## Day 08 — Pipeline 停止协议

### Objective
实现能解除全部已知阻塞的分阶段停止。

### Tasks
- `Stop()` 声明 `noexcept`，五阶段：SignalStop 全部 Node → Abort 全部 Link → InterruptBlockingIo → WakeAllPausedThreads → Join 全部 Node。
- 中间步骤异常转为返回 false，但不中断后续阶段。
- 只有全部 Node 成功 Join 才进 `kStopped`；`stop_in_progress_` 防重入，`stop_succeeded_` 保存首次结果。
- `Pause()`/`Resume()` 只在 `kRunning` 下有效，不引入 `PipelineState::kPaused`。
- 析构函数调 `Stop()`，状态不是 `kStopped` 时 `std::terminate()`。

### Dependencies
Day 07。

### Acceptance Criteria
- 阻塞在空队列 Receive、满队列 Send、暂停等待、外部 IO 的线程都能被 Stop 解除。
- 重复 Stop 返回一致结果且不重复执行流程。
- 存在未 Join 线程时状态停在 `kStopping`，不允许析构。

---

## Day 09 — DAG 生命周期测试与 Sanitizer

### Objective
用确定性条件覆盖全部生命周期边界，并用动态检查确认没有 UB 和数据竞争。

### Tasks
- 建 `tests/dag/` 及 CMake 入口，直接包含 `mediacore/src`，不链接 FFmpeg/Qt；加 `BUILD_MEDIACORE` / `BUILD_APP` 开关。
- 测试脚手架：`TestEvent`（mutex + CV + 序号）、`Probe`（atomic 可观察状态）、`GateNode`（测试方精确控制 Tick 返回时机）。
- 假 Node：EOF Source、Increment Transform、Drain Sink、阻塞收/发 Node、可中断 IO Node、抛异常 Node、具名阻塞 Node。
- 11 个用例：`eof_close_and_drain`、`stop_while_receiving`、`stop_while_sending`、`stop_while_paused`、`pause_resume_pause`、`interrupt_blocking_io`、`partial_start_rollback`、`repeated_stop`、`tick_exception`、`join_before_destruction`、`consumer_starts_first`，每个注册为独立 CTest 并设超时。
- 分别在普通构建、ASan/UBSan、TSan 下跑，用独立构建目录；`ctest --repeat until-fail:N` 暴露低概率竞态。
- 在 `tests/dag/README.md` 记录构建命令、覆盖范围和环境限制。

### Dependencies
Day 08。

### Acceptance Criteria
- 11 个用例在普通构建、警告构建、ASan/UBSan、TSan 下全部通过，不依赖固定时长 sleep。
- 关闭 mediacore 与 app 构建时测试仍可编译运行。
- 任一用例死锁时以超时失败结束。
- 环境不支持某项 Sanitizer 时记录具体原因，不写成已通过。

---

## Day 10 — Packet DTO 与配置系统

### Objective
定义在 Node 之间流动的压缩包对象，并建立全项目唯一的参数来源。

### Tasks
- `av_frame_flag.h`：`AVFrameFlag` 位掩码（`kKeyFrame`/`kEOF`/`kFlush`）+ `constexpr` 位运算重载。
- `packet.h/.cpp`：RAII 封装 `AVPacket*`，构造 `av_packet_alloc()`，析构 `av_packet_free()`。
- 禁拷贝，实现移动构造与移动赋值：窃取指针后源对象置空，移动赋值先释放自身资源并做自赋值检查。
- 携带 `pts_us_`/`dts_us_`/`epoch_`/`flags_` 及读写接口，`IsEmpty()` 判断是否已被移动或分配失败。
- `player_config.h`：按模块分组的嵌套结构（`demux` / `decode` / `audio` / `sync` / `record` / `network` / `jitter`），字段和默认值按第六节预算表定义，默认值一律 `constexpr`。
- 配置加载：可选读配置文件，文件不存在用默认值且不报错；支持环境变量覆盖用于调试。
- `Validate()`：检查数值范围和相互约束（如 `jitter.min_delay_ms <= target <= max_delay_ms`、各容量必须大于 0），非法值返回明确错误而不是静默夹取。
- 本 Day 只定义结构和加载逻辑，各模块接线在各自的 Day 完成。
- CMake 链接 `avutil` 与 `avcodec`。

### Dependencies
Day 01。

### Acceptance Criteria
- 移动后源对象析构不发生 double free，拷贝构造与拷贝赋值编译失败。
- 时间戳单位在注释和接口命名上明确为微秒。
- 不提供任何配置时能拿到一份完整可用的默认配置。
- 配置文件缺失、字段缺失、字段非法三种情况行为明确：前两者回落默认值，后者报错。
- 每个默认值在代码中只有一处定义。

---

# 阶段 B：本地视频最小闭环（Day 11–18）

## Day 11 — 流信息 DTO

### Objective
定义解封装探测结果的表达方式。

### Tasks
- `stream_info.h`：`Rational`（num/den）避免直接暴露 `AVRational`。
- `VideoStreamInfo`（codec_id、宽高、像素格式、帧率、time_base、色彩元数据、stream_index）、`AudioStreamInfo`（codec_id、采样率、声道布局、采样格式、time_base、stream_index）、`MediaInfo`（容器名、总时长微秒、可选轨道信息、是否可 Seek）。
- 用 `std::optional` 表达"没有该轨道"，不用魔数索引。
- 提供从 `AVStream*` 填充的转换函数，集中处理 time_base 与色彩字段。

### Dependencies
Day 10。

### Acceptance Criteria
- 单测覆盖只有视频、只有音频、音视频齐全三种组合。
- 时长和帧率换算有固定输入的期望值比对。
- 头文件在 `src/`，`app` 无法 include。

---

## Day 12 — Demuxer 打开、探测与读包

### Objective
实现可取消的媒体打开，并把容器数据变成带完整元信息的 Packet 流。

### Tasks
- `ffmpeg_demuxer_node.h/.cpp`：双输出，直接继承 `INodeBase`，自持两条 `Link<Packet>`，自己实现 `CloseOutputs()`。
- `Open(path)`：`avformat_open_input` + `avformat_find_stream_info` + `av_find_best_stream`；挂 `interrupt_callback` 读取本 Node 的取消标志；`OnInterruptBlockingIo()` 置该标志；设 Open/Read 超时选项。
- 打开失败返回结构化错误：模块名 + 错误码 + `av_strerror` 文本。
- Open/Read 超时取自 `config.network`，本地文件同样走这套超时路径。
- `Tick()`：一次 `av_read_frame`，按 `stream_index` 分发到 video 或 audio 输出 Link。
- 时间戳一次换算成微秒（`pts * num * 1000000 / den`），`AV_NOPTS_VALUE` 单独处理；epoch 从 `GetCurrentEpoch()` 读取写入每个 Packet；关键帧映射到 `kKeyFrame`。
- 三种返回严格区分：`AVERROR_EOF` → `kEndOfStream`，取消标志生效 → `kStopped`，其他负值 → `kError`。
- 只有一条轨道时另一条 Link 保持未连接，`SendOutput` 安全跳过。

### Dependencies
Day 11。

### Acceptance Criteria
- 固定样本探测出的流信息、Packet 数量、首末 PTS 与 `ffprobe` 一致。
- 双输出各自收到正确 stream 的包。
- 打开不存在的文件返回明确错误，不崩溃也不报 EOF；读取错误不被当作 EOF。
- Open 阻塞中或 Send 阻塞中触发 Stop，能在限定时间内返回。

---

## Day 13 — 帧协议收口与音频帧 DTO

### Objective
统一帧的所有权规则和带内信令，补齐音频侧数据对象和错误表达。

### Tasks
- `VideoFrame` 改用与 `Packet` 相同的 `AVFrameFlag`，删掉重复的 `FrameFlag`；补色彩元数据（`color_space`/`color_trc`/`color_range`/像素格式）；统一 `pts_us` 与 `epoch` 的类型；修正前置声明的 class/struct 不一致。
- 明确 `VideoFrame` 的 move/copy 规则并写进注释：buffer 用 `shared_ptr` 共享，帧对象可拷贝但不深拷像素。
- `audio_frame.h`：RAII 封装 `AVFrame*`，携带 `pts_us`、采样率、声道数、采样格式、样本数、`epoch`、`flags`；禁拷贝、支持移动，规则同 `Packet`。
- 定义结构化错误类型：模块名、错误码、系统/FFmpeg 错误文本、可恢复标志；定义上报通道的接口形状（Node → Pipeline → Facade），本 Day 只定接口不接线。

### Dependencies
Day 10、Day 12。

### Acceptance Criteria
- 全仓库只有一套帧标志枚举，前置声明与定义一致无警告。
- `VideoFrame` 的 RAII 与共享语义、`AudioFrame` 移动后源对象析构安全，都有单测。
- 错误对象能同时表达 FFmpeg 负错误码与系统 errno，且不依赖 Qt。

---

## Day 14 — 软件视频解码

### Objective
把视频 Packet 解成可渲染的帧。

### Tasks
- `video_decoder_node.h/.cpp`：`INode<Packet, VideoFrame>`，用 `VideoStreamInfo` 初始化 `AVCodecContext` 并 `avcodec_open2`。
- send/receive 循环：`avcodec_send_packet` + 循环 `avcodec_receive_frame`，正确处理 `AVERROR(EAGAIN)`。
- 用 `best_effort_timestamp` 换算输出帧 `pts_us`，透传 epoch 与带内标志。
- 收到 `kEOF` 送 nullptr 触发 drain，把解码器内缓存的 B 帧全部取出后再关输出。

### Dependencies
Day 12、Day 13。

### Acceptance Criteria
- 固定样本解出的帧数、分辨率与 `ffprobe` 一致。
- 含 B 帧的样本输出 PTS 单调递增。
- drain 阶段不丢帧。

---

## Day 15 — 像素格式归一

### Objective
保证下游只需处理一种像素布局。

### Tasks
- 检测解码输出的像素格式，非目标格式用 `libswscale` 转换。
- `SwsContext` 按输入格式与尺寸缓存复用，尺寸或格式变化时重建。
- 把 FFmpeg 的行对齐布局按 linesize 逐行拷进 `VideoFrame` 的紧凑布局。
- 保留原始色彩元数据，不在转换中丢失。

### Dependencies
Day 14。

### Acceptance Criteria
- 非 YUV420P 输入的样本也能正确输出。
- 奇数分辨率样本无越界写入（ASan 验证）。
- 分辨率变化时不泄漏 `SwsContext`。

---

## Day 16 — 视频输出节点

### Objective
把解码帧安全地交到 GUI 线程。

### Tasks
- `video_sink_node.h/.cpp`：`INode<VideoFrame, NoPort>`，持 `IVideoSink*`（非拥有）。
- `Tick()` 取帧 → 按 epoch 丢弃陈旧帧 → 调 `OnReceiveVideoFrame`。
- 识别带内 `kEOF`，上报播放完成而不是当作错误。
- Node 只调接口，不碰任何 GL 对象。

### Dependencies
Day 14。

### Acceptance Criteria
- Sink 收到的帧数与 Decoder 输出一致。
- 陈旧 epoch 帧被丢弃且有计数。
- Stop 时阻塞在 Receive 的 Sink 能立即退出。

---

## Day 17 — 纹理上传与色彩基线

### Objective
让真实解码帧正确显示。

### Tasks
- 从 `opengl_view.cpp` 抽出 `app/render/sw_texture_uploader.h/.cpp`，专责三平面上传。
- 首次 `glTexImage2D` 分配，后续 `glTexSubImage2D` 更新，分辨率变化时重新分配。
- 处理 linesize 与宽度不等的情况（`GL_UNPACK_ROW_LENGTH` 或逐行上传）。
- 着色器支持 BT.601 / BT.709 矩阵与 limited/full range 切换，按帧元数据选择。
- 保持"只留最新帧"的信箱策略，宁可丢帧不要延迟。

### Dependencies
Day 16。

### Acceptance Criteria
- 真实媒体首帧与连续帧正确显示，无色偏和错位。
- 全部 GL 调用在 GUI 线程。
- 分辨率切换不花屏不崩溃。

---

## Day 18 — 最小媒体 Pipeline

### Objective
形成第一个真实的端到端垂直闭环。

### Tasks
- `Pipeline` 增加 `Open(url)` 与 `Build()`：探测流信息后按视频轨道自动组图（Demuxer → Decoder → VideoSink）。
- `fsm.h/.cpp`：显式状态机，定义 `PlaybackState` 合法跃迁表，非法跃迁拒绝并记录。
- `player_facade.h/.cpp`：实现 `IPlayer`，持 `unique_ptr<Pipeline>` 和 FSM。
- 串行控制执行器：UI 命令入队后由单一控制线程顺序执行；IO 取消路径可与控制队列并发触发，Stop 不必等队列中的 Open 完成。
- `Reopen`：停止旧 Pipeline 并 Join 后构造全新 Pipeline，不复用已 Abort 的 Link。
- `player_factory.cpp` 改为返回 `PlayerFacade`。

### Dependencies
Day 17。

### Acceptance Criteria
- 本地视频 Open → Play → Pause → Resume → Stop → Reopen 端到端通过。
- 非法跃迁（如未 Open 就 Play）被拒绝且不崩溃。
- Open 阻塞期间触发 Stop 能在限定时间内返回。
- 反复 Open/Stop 20 次无线程泄漏、无 FD 泄漏。

---

# 阶段 C：音频、同步与控制闭环（Day 19–26）

## Day 19 — 音频解码

### Objective
把音频 Packet 解成统一格式的 PCM。

### Tasks
- `audio_decoder_node.h/.cpp`：`INode<Packet, AudioFrame>`，send/receive 与 EAGAIN 处理同视频。
- `libswresample` 重采样到统一输出格式（固定采样率、声道布局、采样格式）。
- 正确处理可变 `nb_samples`，按 `swr_get_out_samples` 分配输出缓冲。
- EOF 时先 drain 解码器，再 `swr_convert` 冲刷重采样器残留样本。
- 输出 `pts_us` 按累计样本数推算，避免逐帧累积误差。

### Dependencies
Day 13、Day 14。

### Acceptance Criteria
- 固定样本解出的总样本数与理论时长一致（容差内）。
- 输出 PTS 单调递增且与样本数自洽。
- 重采样残留样本不丢失。

---

## Day 20 — 音频后端抽象与 ALSA

### Objective
实现可中断的 PCM 输出。

### Tasks
- `iaudio_sink.h`：Open/Write/Drain/Flush/Close/GetDelay 接口，不暴露 ALSA 类型。
- `alsa_audio_sink.h/.cpp`：`snd_pcm_open` + `snd_pcm_hw_params` 参数协商，失败时按优先级回退；设备缓冲大小取自 `config.audio.device_buffer_ms`。
- `snd_pcm_writei` 循环写入并处理部分写。
- `-EPIPE`（xrun）：`snd_pcm_prepare` 后重试并计数；`-ESTRPIPE`（suspend）：`snd_pcm_resume` 或回退到 prepare。
- 写入可中断：Stop 时 `snd_pcm_drop` 让阻塞的 writei 返回。
- CMake 链接系统 `libasound`。

### Dependencies
Day 19。

### Acceptance Criteria
- 参数协商在常见设备上成功，失败时错误信息可定位。
- xrun 后能恢复播放且有计数。
- 阻塞在 writei 时 Stop 能在限定时间内返回。

---

## Day 21 — 音频输出节点与主时钟

### Objective
让音频设备成为播放的时间基准，并让时钟能表达"尚未锚定"。

### Tasks
- `iclock_source.h`：`GetCurrentTimeUs()`、可用性查询，以及 `IsAnchored()` —— 时钟是否已经和真实播放位置对齐。
- `audio_sink_node.h/.cpp`：`INode<AudioFrame, NoPort>`，同时实现 `IClockSource`。
- 用 `snd_pcm_delay` 算出已写入但尚未播出的样本，据此推算真实播放位置；播放位置用 atomic 或加锁保护。
- 锚定语义：起播和 Flush 之后时钟处于未锚定状态，`GetCurrentTimeUs()` 返回待锚定的目标位置且不推进；写入第一个音频帧时用该帧 PTS 完成锚定，之后正常推进。这样 UI 进度条不会在 Seek 后跳回旧位置。
- 暂停时停止写入并让时钟停止推进，恢复后从当前位置继续。
- 按 epoch 丢弃陈旧音频帧；队列与设备缓冲的合计不超过 `audio.max_buffered_ms`。
- 参数取自 `config.audio`，不写魔数。

### Dependencies
Day 20。

### Acceptance Criteria
- 音频-only 播放连续无爆音。
- 暂停期间时钟不推进，恢复后连续。
- 时钟值与实际听感位置误差可测量且在可接受范围内。
- 未锚定期间读到的是目标位置而非旧位置，写入首帧后 `IsAnchored()` 变真。
- 缓冲总量不超过配置上限。

---

## Day 22 — A/V 同步

### Objective
让视频跟随音频主时钟播放，并在时钟未锚定时不阻塞出画。

### Tasks
- `av_sync_node.h/.cpp`：`INode<VideoFrame, VideoFrame>`，构造时注入 `IClockSource*`。
- 算帧 `pts_us` 与主时钟差值：超前等待，落后小于阈值立即送出，落后超阈值丢帧并计数。
- 时钟未锚定时不等待也不丢帧，直接放行 —— 起播和 Seek 后第一帧不被音频阻塞。
- 等待用可中断的 `wait_until`，Stop 或 Pause 时立即返回。
- 视频-only 时 fallback 到单调墙钟，起播时刻对齐首帧 PTS。
- 音频存在但超过 `sync.reanchor_timeout_ms` 仍未锚定（设备卡住、音频轨已结束），退化到墙钟并记 WARN，不永久等待。
- 暴露同步偏差统计：平均偏差、最大偏差、丢帧数、退化到墙钟的次数。

### Dependencies
Day 18、Day 21。

### Acceptance Criteria
- A/V 样本连续播放，同步偏差有数值指标而非目测。
- 等待中触发 Stop 能立即退出。
- 视频-only 播放速率正确。
- 音频设备不可用或音频轨提前结束时，视频仍按墙钟正常播放，不卡死在等待上。

---

## Day 23 — 音视频组图

### Objective
把音频链接入自动组图。

### Tasks
- `Build()` 支持三种拓扑：视频-only、音频-only、音视频齐全。
- 音视频齐全时组：Demuxer → VideoDecoder → AVSync → VideoSink，Demuxer → AudioDecoder → AudioSink。
- `AVSyncNode` 的时钟源在 Build 阶段注入：有音频用 `AudioSinkNode`，否则用墙钟。
- 全部 Link 容量取自 `PlayerConfig`，Build 是唯一读取容量的地方，Node 不自带默认值。
- 保证登记顺序满足 Source → Sink，分支图同样成立。

### Dependencies
Day 22。

### Acceptance Criteria
- 三种拓扑各自能正常播放。
- 单轨道样本不因另一条 Link 未连接而阻塞。
- 启动顺序在分支图上仍是消费者优先。
- 改配置里的容量能改变实际队列深度，代码中无硬编码容量。

---

## Day 24 — 播放控制语义与 EOF 汇聚

### Objective
让暂停恢复在整条链上语义一致，并让"播完了"成为一个明确事件。

### Tasks
- 明确 Pause 的三处语义：ALSA 停止写入但不丢弃已排队样本；时钟停止推进并记录暂停时刻；AVSync 中断当前等待、恢复后重新计算。统一写进注释。
- Demuxer EOF 后经 `CloseOutputs()` 向两条链传播；Decoder 收到输入 EOF 先 drain 再关自己的输出；Sink 排空后判定本路结束。
- Pipeline 汇聚全部 Sink 的结束状态，全部结束才判定播放完成；音频 Sink 结束前调 Drain 让设备缓冲播完。
- 播放完成事件经 Facade 上报，FSM 跃迁到对应状态。

### Dependencies
Day 23。

### Acceptance Criteria
- 连续 50 次快速 Pause/Resume 无爆音、无死锁、无累积漂移，暂停期间 CPU 占用回落。
- 播放到末尾收到且仅收到一次播放完成事件，音频尾部不截断。
- 音频-only、视频-only、音视频三种情况都正确汇聚。

---

## Day 25 — Seek 与同步基准重建

### Objective
实现无残留的跳转，并明确 Seek 之后由谁、在什么时刻重新建立音画同步基准。

### Tasks
- `Pipeline::Seek(timestamp_us)`：递增 epoch → Demuxer 物理定位 → 注入带内 `kFlush` → 恢复读包。
- Demuxer 用 `av_seek_frame` 定位到目标前最近关键帧并 `avformat_flush`。
- Decoder 收到 `kFlush` 调 `avcodec_flush_buffers` 清参考帧。
- AudioSink 收到 `kFlush`：`snd_pcm_drop` 丢设备缓冲 → 时钟转为未锚定并把 seek 目标记为待锚定值 → 写入 flush 后第一个音频帧时用该帧 PTS 完成锚定。
- AVSync 收到 `kFlush` 清内部状态；未锚定期间直接放行第一帧，Seek 立即出画，不等音频。
- 各节点在 Tick 中比较 `item.epoch` 与全局 epoch，小于则丢弃 —— 这是丢弃 pre-seek 数据的唯一手段，队列存量和"在飞"数据都覆盖。
- 兜底：超过 `sync.reanchor_timeout_ms` 未等到音频，或视频-only，AVSync 用墙钟锚定到第一帧 PTS。
- 暂停态 Seek：渲染目标帧后停在暂停态，时钟保持未锚定，Resume 时才锚定。
- last-writer-wins：Seek 进行中收到新 Seek 以最后一次为准，不排队执行。

### Dependencies
Day 24。

### Acceptance Criteria
- 播放态、暂停态、EOF 后 Seek 均能正确跳转。
- Seek 后不出现旧位置的画面或声音；进度显示不回跳到旧位置。
- Seek 后音画同步偏差在重锚定完成后回到正常范围，且没有出现"视频已到新位置、音频仍在旧位置"。
- 音频设备异常或视频-only 时 Seek 仍能出画并按墙钟继续，不卡在等待音频上。
- 暂停态 Seek 显示目标帧且不自动恢复播放。
- 连续快速 Seek 不死锁、不累积延迟。

---

## Day 26 — 错误闭环

### Objective
让所有失败路径都有明确出口。

### Tasks
- 把 Day 13 定义的结构化错误接到实际通道：Node 检测到不可继续的错误 → 记录 `NodeFinishReason` → Pipeline 汇总 → Facade 上报 → FSM 跃迁 `kError`。
- 区分可恢复错误（单帧解码失败，跳过继续）与不可恢复错误（打开失败、设备丢失，终止播放）。
- 未处理异常经 `GetUnhandledException()` 取出并转成错误上报。
- 错误态下 Stop 与 Reopen 仍然可用；UI 层显示错误信息。

### Dependencies
Day 25。

### Acceptance Criteria
- 打开损坏文件、播放中拔掉音频设备、解码失败三种情况都能正确上报并保持进程存活。
- 错误态下 Stop/Reopen 稳定。
- 错误信息包含模块名、错误码和原始文本。

---

# 阶段 D：录制分支与优雅排空（Day 27–30）

## Day 27 — Tee 与分支策略

### Objective
在不改主图拓扑的前提下引出录制分支。

### Tasks
- `tee_node.h`：一进多出，直接继承 `INodeBase`，持多条输出 Link，`CloseOutputs()` 关闭全部。
- 帧用 `shared_ptr` 复制转发，不深拷像素数据。
- 在"构建时固定分支"与"独立录制 Pipeline"中定型一种方案并写明理由。
- 主播放分支用阻塞背压，录制分支按 `config.record.frame_capacity` 设容量、满了丢帧，慢录制不得拖停主播放；丢帧有独立计数可查询。

### Dependencies
Day 26。

### Acceptance Criteria
- 反复开始/停止录制不修改运行中的主图拓扑。
- 人为拖慢录制分支时主播放帧率不下降。
- 丢帧数可查询且与实际一致。

---

## Day 28 — 编码节点

### Objective
把帧编码成 H.264 与 AAC 包。

### Tasks
- `video_encoder_node.h/.cpp`：`INode<VideoFrame, Packet>`；`rate_control_config.h` 集中定义 CBR / CRF / CQP 三种模式参数，并入 `config.record`；分辨率、帧率、GOP、profile/level 从配置和输入帧推导。
- `audio_encoder_node.h/.cpp`：`INode<AudioFrame, Packet>`；重采样到编码器要求的 FLTP 格式与采样率；用 `AVAudioFifo` 攒够固定 `frame_size` 再送编码。
- 两者输入 PTS 都归一化到编码器 time_base 并保证单调；音频按累计样本数归一化。
- EOF 时视频送 nullptr drain；音频先冲 FIFO 残留（不足一帧补静音）再 drain 编码器。

### Dependencies
Day 27。

### Acceptance Criteria
- H.264 包 PTS/DTS 单调且可复现，三种码率模式各自产出符合预期的码率。
- AAC 包时间戳连续无空洞，尾部残留样本不丢失，编码总时长与输入一致（容差内）。
- drain 阶段不丢包。

---

## Day 29 — MP4 封装

### Objective
把编码包写成可播放的文件。

### Tasks
- `mp4_muxer_node.h/.cpp`：双输入，直接继承 `INodeBase`。
- `avformat_alloc_output_context2` + `avformat_new_stream`，从编码器 context 复制参数。
- `av_interleaved_write_frame` 交错写盘，时间戳换算到各流 time_base。
- 支持单轨兼容：只有视频或只有音频时正常写出。
- 双输入取包按 DTS 较小者优先，避免一路阻塞另一路。

### Dependencies
Day 28。

### Acceptance Criteria
- 输出文件能被 `ffprobe` 正确解析。
- 音视频轨道时长一致（容差内）。
- 单轨录制同样产出合法文件。

---

## Day 30 — Drain 协议

### Objective
让录制能优雅结束并写出完整文件。

### Tasks
- `DrainAndStop()`：与立即 Stop 完全分离的停止路径。
- 流程：停止向录制分支送新帧 → 关闭分支 Link（Close 而非 Abort）→ 等编码器和 Muxer 排空 → `av_write_trailer` → 走正常 Join。
- Drain 设超时上限，超时后退化为立即停止并记录文件可能不完整。
- 明确立即 Stop 路径的文件损失边界：已写入部分可能缺 trailer。
- 主播放的立即 Stop 不受录制 Drain 阻塞。

### Dependencies
Day 29。

### Acceptance Criteria
- `DrainAndStop` 后文件有完整 trailer，可 Seek 播放。
- 立即 Stop 后进程正常退出，损失边界符合文档描述。
- Drain 超时路径不死锁。

---

# 阶段 E：VA-API、零拷贝与色彩（Day 31–38）

## Day 31 — 硬件能力探测

### Objective
在写硬解代码前确定环境能力边界。

### Tasks
- 探测 DRM render node、VA-API 设备可用性（`av_hwdevice_ctx_create`）并枚举支持的解码 profile。
- 探测 EGL 扩展（`EGL_EXT_image_dma_buf_import`、`..._modifiers`）和 GL 扩展（`GL_OES_EGL_image_external`）。
- 确认 Qt OpenGL 上下文所在线程与共享上下文的可行性。
- 建立支持矩阵：环境 → 可用能力，输出为可保存的探测报告。

### Dependencies
Day 18。

### Acceptance Criteria
- 不支持的环境明确报告缺失项，可据此跳过后续硬解 Day。
- 支持的环境保存完整探测报告。
- 探测过程不影响软件播放路径。

---

## Day 32 — 硬件帧契约

### Objective
定义硬件帧的所有权和释放规则。

### Tasks
- `hw_device_context.h/.cpp`：RAII 封装 `AVBufferRef*` 硬件设备上下文。
- `VideoFrame` 扩展为 sum type：Software 变体保持现状，Hardware 变体持 `AVFrame*` / `AVBufferRef*` / DMA-BUF fd / descriptor。
- 明确释放线程规则和 fd 所有权（谁 dup、谁 close，禁止双重关闭），写进注释与断言。
- 色彩元数据在两种变体上表达一致。

### Dependencies
Day 31。

### Acceptance Criteria
- 所有权规则有单测或运行期断言覆盖。
- 反复创建销毁硬件帧无 fd 泄漏（`/proc/self/fd` 计数验证）。
- Software 路径不受影响。

---

## Day 33 — VA-API 解码正确性

### Objective
先证明硬解能解对，再谈零拷贝。

### Tasks
- `VideoDecoderNode` 增加硬解路径：`av_hwdevice_ctx_create` + 设 `hw_device_ctx`。
- 实现 `get_format` 回调选 `AV_PIX_FMT_VAAPI`，不支持时回退软件格式。
- 本 Day 先用 `av_hwframe_transfer_data` 把硬件帧下载成软件帧，复用现有渲染路径。
- 硬解初始化失败自动回退软解，回退路径有日志。

### Dependencies
Day 32。

### Acceptance Criteria
- 硬解画面与软解逐帧一致（或在容差内）。
- 不支持的编码格式自动回退且不崩溃。
- 反复切换软/硬解不泄漏。

---

## Day 34 — DMA-BUF 导出

### Objective
拿到可跨 API 共享的帧句柄。

### Tasks
- `vaExportSurfaceHandle` 导出 `VADRMPRIMESurfaceDescriptor`。
- 处理多对象、多平面，逐个记录 fd、offset、stride、modifier。
- fd 所有权纳入 Day 32 定义的 RAII 管理。
- 校验描述符字段与预期布局一致。

### Dependencies
Day 33。

### Acceptance Criteria
- 描述符字段校验通过。
- 连续播放 10 分钟无 fd 泄漏。
- 帧释放时机正确，不提前 close 仍在使用的 fd。

---

## Day 35 — EGLImage 导入

### Objective
把硬件帧直接变成 GL 纹理。

### Tasks
- `app/render/hw_texture_importer.h/.cpp`：`eglCreateImageKHR` 从 DMA-BUF 描述符创建 EGLImage，`glEGLImageTargetTexture2DOES` 绑定到纹理，NV12 按两个平面分别导入。
- EGLImage 的创建和销毁都在 GUI/渲染线程执行。
- 处理 surface 生命周期：GL 用完之前不能释放底层 surface，必要时加 fence。
- 窗口销毁时释放全部 EGLImage。

### Dependencies
Day 34。

### Acceptance Criteria
- 硬解帧正确显示，与软解画面一致。
- 连续播放、Stop/Reopen、窗口销毁均无黑帧和悬垂资源。
- 无跨线程 EGL/GL 调用。

---

## Day 36 — 双路径集成与性能

### Objective
让软硬两条路径共存并用数据证明收益。

### Tasks
- 按 Day 31 的支持矩阵决定默认路径，实现运行中回退：硬解失败切回软解且不中断播放。
- 硬解开关进配置（`auto` / 强制软解 / 强制硬解），`auto` 时按支持矩阵决定；强制硬解在不支持的环境下报明确错误而不是静默回退。
- 性能基准：1080p 与 4K 各测 CPU 占用、GPU 占用、内存、掉帧数、首帧时间。
- 用 `av_hwframe_transfer_data` 调用计数、DMA-BUF fd 存在性、CPU/GPU 数据共同证明零拷贝生效，不以"代码里没有 memcpy"作为证据。
- 明确硬解开启时的录制策略：硬件帧下载后软件编码、加硬件编码器、或暂时禁用录制，三选一并写明。

### Dependencies
Day 35、Day 30。

### Acceptance Criteria
- 性能数据附完整输入、环境、时间源和测量方法。
- 回退路径不崩溃、不泄漏。
- 硬解开启时录制行为明确，不让调用方隐式踩中格式不兼容。

---

## Day 37 — SDR 色彩管理与验证

### Objective
让 SDR 内容颜色正确，并用可复现的方法验证。

### Tasks
- `color_shader.h`：BT.601 / BT.709 / BT.2020 矩阵，limited / full range 量化范围处理。
- 按帧 `color_space` 与 `color_range` 动态选矩阵，元数据缺失时按分辨率推断并写明规则。
- 支持不同采样格式（YUV420P / NV12 / P010）与矩阵的组合。
- 建参考样本集（各色彩空间、各 range 组合），实现离屏渲染取回像素的测试工具，定义容差并逐组合比对。

### Dependencies
Day 35。

### Acceptance Criteria
- 全部组合与参考图在容差内通过，测试可脚本化重复执行且结果稳定。
- 元数据缺失时的默认推断有明确规则。
- 矩阵切换不产生闪烁。

---

## Day 38 — HDR 识别与 Tone Mapping

### Objective
正确解析 HDR 元数据并映射到 SDR 显示。

### Tasks
- 识别 PQ（SMPTE ST 2084）与 HLG，实现对应的 GLSL 反变换。
- 解析 mastering display 与 content light level 元数据。
- HDR 走独立着色器分支，SDR 路径不受影响。
- 实现 Reinhard tone mapping（参数可配置）和 BT.2020 → BT.709 色域转换。
- 明确输出显示假设（目标亮度、色域）并记录所用参数和参考结果。

### Dependencies
Day 37。

### Acceptance Criteria
- HDR 样本正确识别为 PQ 或 HLG，线性化结果与参考实现比对通过。
- HDR 样本显示无明显 clipping 和色偏。
- SDR 样本仍走原路径。
- 参数与参考结果记录完整可复现。

---

# 阶段 F：网络播放与抗弱网（Day 39–43）

## Day 39 — HTTP 播放基线与缓冲恢复

### Objective
用 FFmpeg 的协议层实现可靠的网络播放，并让网络抖动不直接中断播放。

### Tasks
- `FfmpegDemuxerNode` 接受 HTTP URL，复用现有 Open/Read 路径；`rw_timeout`、`timeout`、`user_agent` 取自 `config.network`；interrupt callback 覆盖网络阻塞场景。
- 建本地 HTTP 测试服务和固定样本。
- 定义"缓冲中"与"恢复播放"状态并接入 FSM；网络场景的 Link 容量按 `config.network` 覆盖本地默认值，队列上限有背压。
- 区分可恢复错误（超时、连接重置）与不可恢复错误（404、DNS 失败）；可恢复错误按 `config.network.max_reconnect` 做有限次重连，退避策略可配置，重连期间仍可 Stop。
- `qos_monitor.h/.cpp`：定义并采样首帧时间、rebuffer 次数与时长、队列深度。

### Dependencies
Day 26。

### Acceptance Criteria
- 本地 HTTP 服务下可连续播放，网络错误不被当作 EOF。
- 连接或读取阻塞时 Stop 能在限定时间内返回，重连中触发 Stop 也能及时返回。
- 限速、暂断、慢读三种条件下状态迁移正确，队列不超上限不 OOM，重连次数符合配置。

---

## Day 40 — FFmpeg 能力验收与决策门

### Objective
确定是否需要自研 HTTP 层。

### Tasks
- 验证 Range 请求与网络 Seek、HTTP 重定向、FFmpeg 支持的 HLS 样本播放。
- 对照需求评估缓存观测能力、取消及时性、Range 控制粒度是否足够。
- 形成可复现的能力表和决策记录。只有证明 FFmpeg 无法满足时，才规划 `HttpSourceNode + AVIOContext`（及配套 `byte_chunk.h`）。

### Dependencies
Day 39。

### Acceptance Criteria
- 能力表覆盖 Range、Seek、重定向、HLS 四项且可复现。
- 决策记录明确结论和依据。
- 自定义 HTTP 层不作为本阶段的默认验收项。

---

## Day 41 — RTP 解析与 H.264 解包

### Objective
实现可取消的 UDP 接收和 Access Unit 组装。

### Tasks
- `rtp_source_node.h/.cpp`：`INode<NoPort, RtpPacket>`；`RtpPacket` 含 SSRC、payload type、序列号、时间戳、marker、payload。
- UDP 接收用 `shutdown` 或自管道实现可取消，由 `OnInterruptBlockingIo()` 触发；处理 16 位序列号回绕并维护扩展序列号；校验包长度拒绝截断包。
- `rtp_depacketizer_node.h/.cpp`：`INode<RtpPacket, Packet>`，支持 Single NALU、STAP-A、FU-A 三种模式。
- FU-A 按 start/end 位组装，中间分片丢失时丢弃整个不完整 AU 并计数；按 marker 位判定 AU 边界。
- 输出加 Annex B 起始码或按 AVCC 格式，与解码器约定一致；建录制包 fixture 供回放测试。

### Dependencies
Day 26。

### Acceptance Criteria
- 解析器单测覆盖截断包、乱序、重复、序列号回绕，非法包被拒绝且不崩溃。
- 三种打包模式的解包结果与 fixture 期望一致，不产出半截数据。
- 阻塞在 recv 时 Stop 能立即返回。
- 纯单测覆盖，不依赖真实网络。

---

## Day 42 — Jitter Buffer 与 RTP 闭环

### Objective
实现确定性的重排判丢，并让 RTP 流真正播放出来。

### Tasks
- `jitter_buffer_node.h/.cpp`：`INode<RtpPacket, RtpPacket>`，按扩展序列号排序，窗口深度取自 `config.jitter.target_delay_ms`。
- 重复包丢弃并计数；迟到包（已越过输出位置）丢弃并计数；空洞按 deadline 判丢；容量按 `config.jitter.packet_capacity` 封顶，超限按策略丢最旧数据。
- 时钟抽象为可注入接口，测试用确定性虚拟时钟。
- 组图：`RtpSourceNode → JitterBufferNode → H264DepacketizerNode → VideoDecoderNode → VideoSinkNode`。
- 冷启动等首个可解码 IDR，之前数据丢弃；从带内 SPS/PPS 或带外配置初始化解码器。
- 网络路径与本地路径复用同一套解码、同步和输出节点。

### Dependencies
Day 41、Day 22。

### Acceptance Criteria
- 固定乱序 trace 的输出顺序与期望完全一致，用虚拟时钟验证不依赖 sleep。
- 重复、迟到、空洞三类计数准确，容量上限生效不无限增长。
- H.264 RTP 视频可连续播放，丢 FU 分片时画面不出现花屏堆积。
- Stop 能在限定时间内退出。

---

## Day 43 — Jitter 自适应、恢复与弱网验证

### Objective
在延迟和丢包之间做可控权衡，并用可复现的弱网环境验证。

### Tasks
- 统计网络 jitter 与迟到包比例，在 `config.jitter.min_delay_ms` 与 `max_delay_ms` 之间调整目标缓冲深度，禁止无限增大。
- 处理断流（超时进等待、恢复后重新对齐）、重连与 SSRC 切换（清空 Buffer、重置序列号基准、重新等 IDR）；恢复路径明确状态、超时和"首个可解码帧"判定，时钟重锚定复用 Day 25 的机制。
- 用 netem 或代理建 profile：固定延迟、带宽受限、丢包、乱序、重复、抖动、断流，参数固定可版本化；建固定 trace 与随机种子机制。
- 定义指标口径：首帧时间、稳定延迟、rebuffer 次数与比率、队列深度峰值、恢复时间、丢包率；`QosMonitor` 采集并输出结构化报告，覆盖 HTTP 与 RTP 两条路径。
- 用实测数据校准第六节预算表的初始值，特别是各队列容量、`audio.max_buffered_ms` 和 jitter 三个延迟参数；校准结论写回预算表。
- 首帧时间的统计口径写明测试源 IDR 周期和网络 RTT，不写成无条件承诺。

### Dependencies
Day 42、Day 39。

### Acceptance Criteria
- 固定 trace 下延迟与迟到丢包的取舍可对比，缓冲深度始终在上下限内。
- 断流、重连、SSRC 切换三种恢复路径各有明确验收结果。
- 同一 profile 多次运行结果稳定可比较，环境搭建可脚本化重现。
- 每个 profile 产出 HTTP 与 RTP 的可重复对比报告；报告能体现 Jitter Buffer 没有靠无限增大延迟换取表面平稳。
- 各队列深度峰值和内存占用有实测数据，预算表的初始值据此确认或修正。

---

# 阶段 G：系统收口（Day 44–45）

## Day 44 — 全量回归

### Objective
在完整能力集上做一次系统性回归。

### Tasks
- 建回归矩阵，维度：输入源（本地 / HTTP / RTP）、轨道组合（音频-only / 视频-only / 音视频）、解码路径（软解 / 硬解）、控制操作（播放 / 暂停 / Seek / Stop / Reopen）、录制开关。
- 逐组合执行并记录结果，修复暴露的问题。
- 全矩阵在 ASan/UBSan 与 TSan 下各跑一轮。
- 记录各组合的常驻内存、峰值内存和队列深度峰值，与预算表比对，超出的项要么修代码要么修预算并写明理由。

### Dependencies
Day 43。

### Acceptance Criteria
- 回归矩阵全部通过，Sanitizer 无新增报告。
- 长时间播放（不少于 1 小时）无内存增长、无 fd 泄漏。
- 实测内存和队列峰值不超过预算表，或预算表已按实测更新。

---

## Day 45 — 性能固化与文档收口

### Objective
固定性能基线并让文档与代码一致。

### Tasks
- 固化性能表：首帧时间、稳定延迟、同步偏差、rebuffer 比率、CPU/GPU 占用、内存、队列峰值、FD 数量，每项附输入样本、环境、时间源和测量方法。
- 把 Day 43、Day 44 实测校准后的数值写回第六节预算表，作为默认配置的最终值。
- 更新 README 的架构描述与目录结构，更新 ARCHITECTURE 补齐新落地组件的实现协议。
- 删除所有无法由正式构建与测试复现的描述。

### Dependencies
Day 44。

### Acceptance Criteria
- 性能表完整且可复现。
- 预算表的每个数字都有实测依据，代码默认值与预算表一致。
- 三份文档描述与代码一致，无过期内容。
- 新开发者按 README 能独立完成构建与测试。

---

# Final Project Acceptance

全部 Day 完成后，项目应同时满足以下条件。

## 功能能力

| 能力 | 标准 |
|---|---|
| 本地播放 | 音视频文件可 Open / Play / Pause / Resume / Stop / Reopen，音频-only 与视频-only 同样成立 |
| 音画同步 | 以音频设备时钟为主时钟，同步偏差有数值指标；视频-only 使用墙钟且速率正确 |
| Seek | 播放态、暂停态、EOF 后均可跳转，无旧画面与旧声音残留，同步基准正确重建，连续快速 Seek 不死锁 |
| EOF | 播放到末尾产生且仅产生一次播放完成事件，音频尾部不截断 |
| 错误处理 | 打开失败、设备丢失、解码失败均有结构化上报，进程存活，Stop/Reopen 可用 |
| 录制 | 可在不改主图拓扑的前提下录制 MP4，Drain 后文件有 trailer 且可 Seek，慢录制不拖停播放 |
| 硬件解码 | VA-API 解码正确，不支持时自动回退软解，零拷贝路径有数据证明收益 |
| 色彩管理 | SDR 各色彩空间与 range 组合通过参考比对；HDR 经 tone mapping 显示无明显 clipping |
| HTTP 播放 | 可连续播放，阻塞时可及时停止，抖动下有缓冲与有限重连 |
| RTP 播放 | H.264 RTP 可播放，乱序、重复、丢包、断流、重连各有明确策略和验收结果 |
| 弱网验证 | 固定 profile 下 HTTP 与 RTP 均能产出可重复、可比较的指标报告 |
| 配置 | 全部可调参数集中在 `PlayerConfig`，有单一默认值来源；无配置文件时能正常运行 |
| 资源控制 | 全部队列、缓冲和缓存有上限，实测峰值不超预算表；长时间运行内存不增长 |
| 可观测性 | 全模块统一日志，含时间戳、线程 id、模块名、级别和源码位置；错误可定位到模块和错误码 |

## 工程标准

- 全部代码在正式目录并纳入 CMake target，PUBLIC/PRIVATE 头文件边界正确。
- `app` 层无法 include 任何私有头，边界由构建系统强制而非约定。
- 全部测试为确定性测试，不用 sleep 证明并发正确，每个用例有超时。
- 普通构建、警告构建、ASan/UBSan、TSan 全部通过。
- 长时间运行无内存增长、无线程泄漏、无 FD 泄漏。
- 全部 `.h`/`.cpp` 有 Doxygen 注释，类和函数说明职责、线程模型与生命周期。
- 代码通过 clang-format 与 clang-tidy 检查。
- 代码中不存在未经 `PlayerConfig` 的可调魔数。

## 架构标准

- 四个状态域保持分离，未被合并成全局状态。
- Node 不创建 Link、不感知完整拓扑、不管理自身线程生命周期。
- 媒体数据只经 `Link<T>` 单向流动，无跨 Node 回调链。
- 立即停止、正常 EOF、录制 Drain 三条路径语义严格区分，未互相复用。
- 运行中的主 Pipeline 拓扑从未被修改。
- 全部外部阻塞调用支持取消或超时。
- 配置在 Build 前冻结，运行中不修改。
- 同步基准的建立和重建只有一处实现，起播与 Seek 复用同一套锚定逻辑。

## 文档标准

- README 描述项目定位、目录结构、高层架构与构建方式，与最终代码一致。
- ARCHITECTURE 描述全部已实现组件的实现协议，无未实现内容的伪协议。
- 本计划书与最终交付范围一致，范围调整已回写本文。
- 全部性能结论附输入、环境、时间源和测量方法。

满足以上全部条件，项目视为完成。

