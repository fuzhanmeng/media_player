# AI Media Player 架构设计文档

> 本文档统一定义整个播放器的**分层、职责边界、所有权模型、控制流/数据流/生命周期**，
> 是后续所有编码（D9 及以后）的唯一权威依据。任何与本文冲突的代码都应以本文为准修正。
>
> 工程哲学：**DAG 数据流 + 分层解耦 + RAII 所有权 + 单一职责**。

---

## 0. 三条正交的轴

理解本架构的钥匙：把系统拆成**三条互不干扰的轴**，每条轴回答一个独立问题。

| 轴 | 回答的问题 | 载体 | 方向 |
|----|-----------|------|------|
| **数据流** | 像素/采样怎么从文件流到屏幕？ | `Link<T>` 有界队列 | Source → Sink（单向） |
| **控制流** | 播放/暂停/Seek 指令怎么下达？ | `SyncContext`（全局态+epoch）+ 节点停止标志 | UI → Player → Pipeline → 各 Node |
| **生命周期** | 谁创建/销毁谁？谁 join 谁？ | 所有权（unique/shared/裸指针） | 由外向内创建，由内向外销毁 |

**三轴分离**是本架构的核心：数据流只管搬运，控制流只管发令，生命周期只管存亡。
一个 Bug 若跨轴（例如"停止"既要停数据又要管销毁），就要拆开到各自的轴上分别处理。

---

## 1. 分层与职责边界

自顶向下五层，每层只依赖其下一层的**抽象**，不感知实现：

```
┌──────────────────────────────────────────────────────────┐
│  app/ (Qt GUI)   PlayerWidget / OpenGLView / Controller   │  UI 层
│      依赖 ↓ 仅 IPlayer + IVideoSink 两个纯接口               │
├──────────────────────────────────────────────────────────┤
│  PlayerFacade (implements IPlayer)                         │  门面层
│      翻译 UI 指令 → Pipeline 操作；持有 FSM 校验状态跃迁      │
├──────────────────────────────────────────────────────────┤
│  Pipeline                                                  │  编排层
│      Build 组图 / Start / Stop / Seek；拥有全部 Node 与 Link │
├──────────────────────────────────────────────────────────┤
│  INodeBase / INode<InT,OutT>  +  各具体 Node                │  节点层
│      每节点 1 线程；标准三态循环；只做"处理一个数据项"         │
├──────────────────────────────────────────────────────────┤
│  Link<T> / ThreadSafeQueue<T> / SyncContext                │  基石层
│      数据通道（背压队列）+ 控制总线（atomic 态+epoch）        │
└──────────────────────────────────────────────────────────┘
```

### 每个类"负责什么 / 不负责什么"

| 类 | ✅ 负责 | ❌ 不负责 |
|----|--------|----------|
| **IPlayer**（接口） | 定义 UI 可见的能力契约（Open/Play/Pause/Stop/Seek/状态查询/Sink 注入） | 任何实现细节；不知道 Pipeline/Node 存在 |
| **IVideoSink**（接口） | 定义"接收一帧"的契约 `OnReceiveVideoFrame` | 帧从哪来；解码/同步细节 |
| **PlayerFacade** | 实现 IPlayer；把 UI 指令翻译成 Pipeline 调用；持 FSM 校验跃迁合法性；创建/销毁 Pipeline | 不碰线程、不碰队列、不解码；不做具体音视频处理 |
| **FSM** | 维护合法状态跃迁表（如 kStopped 不能直接 Pause）；拒绝非法跃迁 | 不执行动作，只判定"这个跃迁允不允许" |
| **Pipeline** | Build 拓扑（造 Node、造 Link、连线）；按序 Start/Stop；持有全部 Node 和 Link 的所有权；创建唯一的 SyncContext | 不定义节点内部怎么处理数据；不感知 UI |
| **SyncContext** | 持有全局运行态（Running/Paused/Stopped）+ Seek epoch；提供切态与"暂停等待"；notify 暂停 CV | 不持有任何 Node/Link；不知道有几个节点；不做数据处理 |
| **INodeBase** | 单节点线程生命周期（Start/RequestStop/WaitForStop）；持 shared_ptr<SyncContext> 读全局态；标准循环骨架的停止/暂停判定 | 不知道数据类型；不知道上下游是谁 |
| **INode<InT,OutT>** | 强类型输入/输出 Link 的持有与连接；标准三态循环 ThreadMain；暴露纯虚 Tick() | 不定义"一个数据项具体怎么处理"（交给子类 Tick） |
| **具体 Node**（Demuxer/Decoder/…） | 实现 Tick()：收一个→处理→发一个；epoch 过滤陈旧帧 | 不管线程怎么起停（基类管）；不管队列并发（Link 管） |
| **Link\<T>** | 强类型语义管道：Send/Receive/CloseInput/Abort | 不管背压/锁细节（转发给队列）；不管 epoch 过滤（各节点自己比） |
| **ThreadSafeQueue\<T>** | 有界阻塞队列：双 CV 背压、Push/Pop、Close 排空、Abort 丢弃 | 不知道 T 的语义；不知道 DAG 拓扑 |

---

## 2. 所有权模型（本文档最重要的一节）

**核心原则：由外向内创建，由内向外销毁；用 `unique_ptr` 表达独占，`shared_ptr` 表达共享，裸指针表达"只看不拥有"。**

### 2.1 一张所有权全景图

```
                        std::unique_ptr<IPlayer>
   app (main)  ─────────────────────────────────────▶  PlayerFacade
      │  own（独占，工厂返回 unique_ptr）                    │
      │                                                     │ own: std::unique_ptr<Pipeline>
      │  IVideoSink*（裸指针，只引用不拥有）                  ▼
   OpenGLView  ◀───────────────────────────────────  Pipeline
   （app own，                 IVideoSink* 注入          │
    Qt 父子树管理）                                       │ own: vector<unique_ptr<INodeBase>>  （拥有全部节点）
                                                         │ own: vector<shared_ptr<Link<T>>>    （拥有全部连线）
                                                         │ own: shared_ptr<SyncContext>        （创建者，也是共享者之一）
                                                         ▼
                              各 Node ──── shared_ptr<SyncContext> ────┐
                                 │                                     │ 所有节点 + Pipeline
                                 │  each holds                         │ 共享同一个 SyncContext
                                 │  shared_ptr<Link<T>> (in/out)       │
                                 ▼                                     ▼
                              Link<T>  ◀── 被上下游两个节点共享 ──▶  SyncContext（引用计数 = 节点数 + 1）
```

### 2.2 逐条回答所有权问题

**Q: 谁创建 Pipeline？谁销毁 Pipeline？**
- **创建**：`PlayerFacade::Open()` 时创建 `std::unique_ptr<Pipeline>`。Facade 独占拥有它。
- **销毁**：`PlayerFacade::Stop()` 时 `pipeline_.reset()`，或 Facade 自身析构时随 unique_ptr 自动析构。
- **为何是 unique_ptr**：同一时刻只有一条 Pipeline，Facade 是它唯一的拥有者，独占语义最贴切，析构确定性最强（RAII）。

**Q: 谁拥有 SyncContext？**
- **创建者 = Pipeline**（`Build()` 时 `std::make_shared<SyncContext>()`）。
- **拥有者 = Pipeline + 所有 Node**（共享）。引用计数 = 节点数 + 1（Pipeline 自己也留一份，用于 Seek 时 BumpEpoch、停止时 WakeAllPausedThreads）。
- **为何是 shared_ptr 而非 Pipeline 独占**：见下条。

**Q: Node 为什么持有 `shared_ptr<SyncContext>` 而不是裸指针或引用？**
- **共享语义真实存在**：一个 SyncContext 被 N 个节点 + Pipeline 同时使用，这是"多方共享同一对象"的教科书场景。
- **生命周期安全**：节点线程可能在 Pipeline 开始析构后仍在做最后一轮循环（尚未 join 完）。若节点持裸指针，而 SyncContext 已被销毁，就是**悬垂指针 → UB**。shared_ptr 保证"只要还有一个节点活着，SyncContext 就不会死"，天然规避这个竞态。
- **为何不用引用 `SyncContext&`**：引用要求被引用对象在节点整个生命周期内都存活，把生命周期耦合交给了程序员心算，出错即 UB；shared_ptr 用引用计数把这件事自动化。

**Q: Link 是谁创建、谁共享？**
- **创建者 = Pipeline**（`Build()` 时按拓扑 `std::make_shared<Link<T>>(capacity)`）。
- **共享者 = 一条 Link 两端的两个节点**：上游节点 `ConnectOutput(link)`，下游节点 `ConnectInput(link)`，两者各持一个 `shared_ptr<Link<T>>` 指向同一对象。引用计数通常 = 2（+ Pipeline 若也留存）。
- **为何是 shared_ptr**：一条边天然被两个节点共享，且"谁先析构"不确定。shared_ptr 让 Link 随**最后一个**持有者析构，规避"队列被谁释放"的竞态。这也解释了 Tee 多播为何零拷贝——转发的是 `shared_ptr` 引用计数，不是像素。

**Q: `IVideoSink*` 为什么是裸指针而不是 shared_ptr？**
- **所有权不属于 mediacore**：视频接收端（OpenGLView）是 **UI 层（app）创建并拥有**的 Qt Widget，其生命周期由 Qt 的父子对象树管理。mediacore 只是"把帧推给它"，**借用而非拥有**。
- **裸指针正确表达"非拥有观察者（non-owning observer）"**：如果这里用 shared_ptr，mediacore 就参与了 OpenGLView 的生命周期管理——这既不符合事实（Qt 才是它的主人），又会造成 UI 对象被内核延寿的诡异问题。
- **安全边界**：UI 负责在销毁 OpenGLView 前调用 `DetachDisplayView(sink)` 解绑，内核推帧处用 `mutex` 保护 sink_ 读写（见 StubPlayer 的 `sinkMutex_`）。这是"裸指针 + 显式解绑 + 加锁"的经典观察者模式，而非所有权共享。

### 2.3 所有权速查表

| 对象 | 所有者 | 表达方式 | 理由 |
|------|--------|----------|------|
| IPlayer/PlayerFacade | app (main) | `unique_ptr<IPlayer>`（工厂返回） | 独占，确定性析构 |
| Pipeline | PlayerFacade | `unique_ptr<Pipeline>` | 同时只一条，独占 |
| SyncContext | Pipeline 创建，Pipeline+全部 Node 共享 | `shared_ptr<SyncContext>` | 多方共享 + 跨析构存活 |
| 各 Node | Pipeline | `vector<unique_ptr<INodeBase>>` | Pipeline 独占节点，异构统一保管 |
| Link\<T> | Pipeline 创建，两端节点共享 | `shared_ptr<Link<T>>` | 一条边被两节点共享 |
| ThreadSafeQueue\<T> | Link（内嵌成员） | 值成员（组合） | 队列是 Link 的一部分，同生共死 |
| IVideoSink (OpenGLView) | app (UI 层) | `IVideoSink*` 裸指针（内核侧） | 内核借用不拥有，Qt 管其生命周期 |

---

## 3. 类关系 UML

```mermaid
classDiagram
    class IPlayer {
        <<interface>>
        +AttachDisplayView(IVideoSink*)
        +DetachDisplayView(IVideoSink*)
        +Open(url)
        +Play()
        +Pause()
        +Stop()
        +Seek(ts_us)
        +GetState() PlaybackState
    }
    class IVideoSink {
        <<interface>>
        +OnReceiveVideoFrame(shared_ptr~VideoFrame~)
    }
    class PlayerFacade {
        -unique_ptr~Pipeline~ pipeline_
        -FSM fsm_
        -IVideoSink* sink_
        +Open/Play/Pause/Stop/Seek()
    }
    class FSM {
        +CanTransit(from,to) bool
        +Transit(to)
    }
    class Pipeline {
        -vector~unique_ptr~INodeBase~~ nodes_
        -vector~shared_ptr~Link~~ links_
        -shared_ptr~SyncContext~ sync_
        +Build()
        +Start()
        +Stop()
        +Seek(ts_us)
    }
    class SyncContext {
        -atomic~PipelineState~ state_
        -atomic~uint64~ epoch_
        -mutex pauseMutex_
        -condition_variable pauseCv_
        +GetState() / SetRunning/SetPaused/SetStopped()
        +WaitWhilePaused()
        +GetEpoch/BumpEpoch()
    }
    class INodeBase {
        #shared_ptr~SyncContext~ sync_
        -thread thread_
        -atomic~bool~ running_
        -atomic~bool~ stopRequested_
        +Start/RequestStop/WaitForStop()
        #ThreadMain()* / BeforeThreadStart() / OnStopRequested()
    }
    class INode~InT,OutT~ {
        #shared_ptr~Link~InT~~ input_
        #shared_ptr~Link~OutT~~ output_
        +ConnectInput/ConnectOutput()
        #ThreadMain() final
        #Tick()* bool
    }
    class Link~T~ {
        -ThreadSafeQueue~T~ queue_
        +Send/Receive/CloseInput/Abort()
    }
    class ThreadSafeQueue~T~ {
        -queue~T~ queue_
        -mutex mutex_
        -condition_variable notFull_/notEmpty_
        +Push/Pop/Close/Abort()
    }
    class DemuxerNode
    class VideoDecoderNode
    class AVSyncNode
    class VideoSinkNode

    IPlayer <|.. PlayerFacade : implements
    IVideoSink <|.. OpenGLView : implements
    PlayerFacade *-- Pipeline : owns (unique_ptr)
    PlayerFacade *-- FSM : owns
    PlayerFacade ..> IVideoSink : borrows (raw ptr)
    Pipeline *-- INodeBase : owns N (unique_ptr)
    Pipeline o-- Link : owns edges (shared_ptr)
    Pipeline o-- SyncContext : creates+shares (shared_ptr)
    INodeBase <|-- INode : extends
    INode <|-- DemuxerNode
    INode <|-- VideoDecoderNode
    INode <|-- AVSyncNode
    INode <|-- VideoSinkNode
    INodeBase o-- SyncContext : shares (shared_ptr)
    INode o-- Link : in/out (shared_ptr)
    Link *-- ThreadSafeQueue : composition (value)
    VideoSinkNode ..> IVideoSink : pushes frames (raw ptr)
```

图例：`*--` 组合/独占拥有，`o--` 聚合/共享，`..>` 依赖/借用，`<|--` 继承，`<|..` 实现接口。

---

## 4. 数据流图（Source → Sink）

**单向、强类型、有界背压。** 每个方框是一个节点（独立线程），每条箭头是一个 `Link<T>`（有界队列）。
Link 上标注的是流经的数据类型和建议队列深度（深度按数据体积反比：Packet 深、Frame 浅）。

```mermaid
flowchart LR
    subgraph src[数据源层]
      SRC[SourceNode<br/>File/Http/Rtp<br/>只出不进]
    end
    subgraph demux[解封装]
      DMX[DemuxerNode<br/>libavformat<br/>一进双出]
    end
    subgraph dec[解码]
      VDEC[VideoDecoderNode<br/>软解/VA-API]
      ADEC[AudioDecoderNode<br/>+ swr 重采样]
    end
    subgraph sync[同步]
      AVS[AVSyncNode<br/>注入 IClockSource<br/>丢/等/送]
    end
    subgraph sink[终端输出]
      VSINK[VideoSinkNode<br/>推 IVideoSink]
      ASINK[AudioSinkNode<br/>IAudioSink+IClockSource<br/>音频主时钟]
    end

    SRC -->|Link ByteChunk / 深| DMX
    DMX -->|Link Packet 视频 / 深 ~60| VDEC
    DMX -->|Link Packet 音频 / 深 ~60| ADEC
    VDEC -->|Link VideoFrame / 浅 ~3| AVS
    ADEC -->|Link AudioFrame / 中 ~15| ASINK
    AVS -->|Link VideoFrame / 浅 ~3| VSINK
    ASINK -.->|IClockSource 主时钟读数| AVS
    VSINK -.->|IVideoSink* 推帧,越出内核边界| UI[(OpenGLView<br/>app/UI)]

    classDef node fill:#e8f0fe,stroke:#4285f4;
    class SRC,DMX,VDEC,ADEC,AVS,VSINK,ASINK node;
```

**要点：**
- 实线 = 数据流（`Link<T>` 队列，携带 epoch 字段）；虚线 = 非数据依赖（时钟读数 / 跨边界推帧）。
- **音频主时钟**：AudioSinkNode 实现 IClockSource，AVSyncNode 注入它、读音频播放进度作为同步基准（音频不可停顿，故做主）。
- **epoch 过滤**：每个节点 Tick() 里比较 `item.epoch < sync_->GetEpoch()` → 丢弃陈旧帧（Seek 后旧帧自然消散）。
- **背压**：任一下游变慢 → 其输入 Link 填满 → 上游 Send 阻塞 → 逐级传导，防止 OOM。

---

## 5. 控制流图（UI → Player → Pipeline → SyncContext → Node）

**控制流与数据流方向相反且正交。** 指令自上而下传播，最终落到两个终点：SyncContext（全局态）或节点停止标志（局部）。

```mermaid
flowchart TD
    UI[UI: 按钮点击<br/>Play/Pause/Seek/Stop] -->|调用 IPlayer 接口| FAC[PlayerFacade]
    FAC -->|1. FSM 校验跃迁合法性| FSM{FSM<br/>允许?}
    FSM -->|非法, 拒绝| UI
    FSM -->|合法| PIPE[Pipeline]

    PIPE -->|Play/Pause: 改全局态| SC[SyncContext]
    PIPE -->|Seek: BumpEpoch| SC
    PIPE -->|Stop: WakeAllPausedThreads + 逐个 RequestStop| NODES

    SC -->|state_ / epoch_<br/>N 个线程高频无锁读| NODES[各 Node 工作线程]
    SC -.->|WakeAllPausedThreads 纯 notify 踹醒| NODES

    NODES -->|循环顶部读态:<br/>Running→干活<br/>Paused→睡CV<br/>Stopped(节点标志)→退出| LOOP[ThreadMain 三态循环]

    classDef ctrl fill:#fce8e6,stroke:#ea4335;
    class SC,FSM ctrl;
```

**两条落点（方案甲）：**
1. **全局态**（Play/Pause/Seek）→ 写 `SyncContext`（state/epoch），所有节点循环下一轮读到即响应。**一声令下，全体同步**。
2. **停止**（Stop）→ 双管齐下：先逐个 `RequestStop()`（置节点标志 `stopRequested_` + Abort 队列 CV），再 `WakeAllPausedThreads()`（纯 notify 暂停 CV，不改状态）。**顺序关键：先置标志，后唤醒**，否则谓词看不到停止信号会再睡回去。

---

## 6. 五大操作时序图

> 参与者统一：`UI` / `Facade`(PlayerFacade+FSM) / `Pipeline` / `SyncContext` / `Node`(代表所有工作线程)。

### 6.1 Open —— 组图并进入就绪

```mermaid
sequenceDiagram
    participant UI
    participant Facade
    participant Pipeline
    participant SyncContext
    participant Node

    UI->>Facade: Open(url)
    Facade->>Facade: FSM 校验 (kIdle/kStopped → kOpening 合法?)
    Facade->>Facade: state = kOpening
    Facade->>Pipeline: new unique_ptr<Pipeline>(url)
    activate Pipeline
    Pipeline->>SyncContext: make_shared<SyncContext>() (state=Running 初值)
    Pipeline->>Pipeline: 探测流信息, 按拓扑造 Node + Link
    Pipeline->>Node: 每个 Node 注入 shared_ptr<SyncContext>
    Pipeline->>Node: ConnectInput/ConnectOutput 连线
    deactivate Pipeline
    Facade->>Facade: state = kReady (或直接就绪)
    Facade-->>UI: 打开完成 (回调/状态查询可见)
    Note over Facade,Node: 此时线程尚未 Start, 只是组好了图
```

### 6.2 Play —— 启动/恢复

```mermaid
sequenceDiagram
    participant UI
    participant Facade
    participant Pipeline
    participant SyncContext
    participant Node

    UI->>Facade: Play()
    Facade->>Facade: FSM 校验 (kReady/kPaused → kPlaying 合法?)
    alt 首次播放 (线程未起)
        Facade->>Pipeline: Start()
        Pipeline->>SyncContext: SetRunning()
        Pipeline->>Node: 逆数据流逐个 Start() (先消费者后生产者)
        Node->>Node: BeforeThreadStart() 后拉起线程, 进入 ThreadMain 循环
    else 从暂停恢复 (线程在睡)
        Facade->>Pipeline: Resume()
        Pipeline->>SyncContext: SetRunning()
        SyncContext->>Node: pauseCv_.notify_all() 踹醒暂停中的线程
        Node->>Node: WaitWhilePaused 返回, 回循环顶继续 Tick()
    end
    Facade->>Facade: state = kPlaying
    Facade-->>UI: 播放中
    Note over Node: 循环: Running→Tick 干活; 数据开始流动
```

### 6.3 Pause —— 暂停（不退出）

```mermaid
sequenceDiagram
    participant UI
    participant Facade
    participant Pipeline
    participant SyncContext
    participant Node

    UI->>Facade: Pause()
    Facade->>Facade: FSM 校验 (kPlaying → kPaused 合法?)
    Facade->>Pipeline: Pause()
    Pipeline->>SyncContext: SetPaused()  (仅置态, 不 notify)
    Facade->>Facade: state = kPaused
    Facade-->>UI: 已暂停 (主线程立即返回, 不阻塞 UI)
    Note over Node: 各线程下一轮循环顶读到 kPaused
    Node->>SyncContext: WaitWhilePaused() 睡入 pauseCv_ (0% CPU)
    Note over Node: 线程挂起, 不退出; 数据流冻结, 队列存量保留
```

### 6.4 Seek —— 跳转（无锁 epoch）

```mermaid
sequenceDiagram
    participant UI
    participant Facade
    participant Pipeline
    participant SyncContext
    participant Node

    UI->>Facade: Seek(ts_us)
    Facade->>Facade: FSM 校验 (kPlaying/kPaused → 可 Seek?)
    Facade->>Pipeline: Seek(ts_us)
    Pipeline->>SyncContext: BumpEpoch()  (epoch 5→6, 一次原子加)
    Pipeline->>Pipeline: 向 Source/Demuxer 下达带内 kFlush + av_seek_frame
    Pipeline->>Node: (可选) Abort 各 Link 快速排空在飞旧帧
    Note over Node: 旧帧 item.epoch=5 < 全局 epoch=6 → 各节点 Tick 中自然丢弃
    Note over Node: 新位置帧 epoch=6 → 放行, 顺流抵达屏幕
    Facade-->>UI: Seek 完成 (状态不变: 原 Playing 仍 Playing, 原 Paused 仍 Paused)
    Note over SyncContext,Node: 无需节点间握手, 一个计数器沿数据流自扩散
```

### 6.5 Stop —— 停止并拆图（三卡点唤醒 + 反向回收）

```mermaid
sequenceDiagram
    participant UI
    participant Facade
    participant Pipeline
    participant SyncContext
    participant Node

    UI->>Facade: Stop()
    Facade->>Facade: FSM 校验 (任意活动态 → kStopped)
    Facade->>Pipeline: Stop()
    Note over Pipeline: 【关键】先置标志,后唤醒(顺序反了会死锁)
    loop 逐个节点(反向)
        Pipeline->>Node: RequestStop() 置 stopRequested_ + OnStopRequested→AbortLinks()
        Note over Node: 睡在输入队列(Receive/卡点B)与输出队列(Send/卡点C)的线程均被 Abort 叫醒
    end
    Pipeline->>SyncContext: WakeAllPausedThreads() 纯 notify pauseCv_(卡点A, 不改状态)
    Note over Node: 睡在 pauseCv_ 的线程被叫醒,谓词 should_stop()=true → 真返回
    loop 逐个节点
        Node->>Node: 循环顶 IsStopRequested() → break, 线程函数返回
        Pipeline->>Node: WaitForStop() → thread.join() 成功回收
    end
    Pipeline-->>Facade: 全部线程已 join
    Facade->>Facade: pipeline_.reset()  销毁 Pipeline (连带 Node/Link/SyncContext 依次析构)
    Facade->>Facade: state = kStopped
    Facade-->>UI: 已停止
    Note over Pipeline,SyncContext: SyncContext 引用计数归零(节点全销毁+Pipeline释放) → 最后析构
```

---

## 7. 生命周期图（创建 → 启动 → 暂停 → 恢复 → 停止 → 销毁）

### 7.1 单个节点线程的状态机

```mermaid
stateDiagram-v2
    [*] --> Created: Pipeline 构造节点 (注入 SyncContext, 连 Link)
    Created --> Running: Start() → BeforeThreadStart → 拉起线程
    Running --> Paused: 读到 kPaused → WaitWhilePaused (睡 CV, 注入 should_stop)
    Paused --> Running: SetRunning → notify → 醒来回循环顶
    Running --> Stopping: IsStopRequested() → break
    Paused --> Stopping: WakeAllPausedThreads 唤醒 + 谓词 should_stop()=true → break
    Running --> Stopping: Tick() 返回 false (上游 EOF/Abort)
    Stopping --> Joined: WaitForStop() → thread.join()
    Joined --> [*]: 节点对象析构 (unique_ptr 释放)
```

### 7.2 整体对象生命周期时间线

```
时间 ───────────────────────────────────────────────────────────────▶

app 启动
  └─ CreatePlayer() ──▶ PlayerFacade 诞生 (unique_ptr, 存活至 app 退出)
                          │
   Open() ───────────────┼─▶ Pipeline 诞生 (unique_ptr)
                          │     ├─ SyncContext 诞生 (shared, rc=1)
                          │     ├─ Link×M 诞生 (shared, rc=2 each)
                          │     └─ Node×N 诞生 (unique, 各持 SyncContext → rc=N+1)
                          │
   Play() ────────────────┼─▶ Node 线程全部 Start (Created→Running)
   Pause()/Play() ────────┼─▶ Running⇄Paused 往返 (对象不生不灭, 只是线程睡/醒)
   Seek() ────────────────┼─▶ epoch++ (对象不动, 只是旧数据被丢)
                          │
   Stop() ────────────────┼─▶ 全部 Node 线程 Joined
                          │     └─ pipeline_.reset():
                          │          Node×N 析构 (SyncContext rc: N+1→1)
                          │          Link×M 析构
                          │          SyncContext 析构 (rc:1→0, Pipeline 最后一份释放)
                          │
app 退出 ─────────────────┴─▶ PlayerFacade 析构 (若未 Stop, 析构中兜底 Stop)
```

**关键生命周期保证：**
- **RAII 层层嵌套**：app 持 Facade，Facade 持 Pipeline，Pipeline 持 Node/Link/SyncContext。任一层析构自动触发下层析构，无需手动 delete。
- **线程 RAII**：`INodeBase` 析构函数兜底 `RequestStop(); WaitForStop();`，保证线程一定被 join，杜绝 `std::terminate`。
- **SyncContext 后死**：因所有节点共享它，它一定在**最后一个节点析构后**才归零销毁——节点线程收尾时读它绝对安全。这正是"节点持 shared_ptr 而非裸指针"的价值兑现。

---

## 8. 双停止机制详解（方案甲：停止归 Node，注入停止判据）

一个工作线程可能睡死在**三个卡点之一**，停止必须全部踹醒：

| 卡点 | 睡眠原因 | 等待的 CV | 由谁唤醒 |
|------|---------|-----------|---------|
| **A** | 暂停 (state==kPaused) | `SyncContext::pauseCv_` | `WakeAllPausedThreads()` 的纯 `notify_all` |
| **B** | 输入管道**空**，等收 (`input_->Receive()`) | `ThreadSafeQueue::notEmpty_` | `input_->Abort()`（经 RequestStop→OnStopRequested→AbortLinks） |
| **C** | 输出管道**满**，等发 (`output_->Send()`) | `ThreadSafeQueue::notFull_` | `output_->Abort()`（同上） |

**三个卡点，两个唤醒源**：

```
Pipeline::Stop
 ├─ ① 逐个 node->RequestStop()
 │      → stopRequested_ = true
 │      → OnStopRequested() → INode::AbortLinks()
 │            ├─ input_->Abort()    踹醒卡点 B（等收）
 │            └─ output_->Abort()   踹醒卡点 C（等发）
 │
 ├─ ② sync_->WakeAllPausedThreads()   踹醒卡点 A（暂停）
 │
 └─ ③ 逐个 node->WaitForStop()  → join
```

缺任一唤醒源，对应卡点的线程永远醒不来、回不到循环顶去检查 `IsStopRequested()`，`join()` 即永久挂死。

### 为什么卡点 C 必须由框架兜住（而非交给派生类自觉）

卡点 C 是**最容易被漏掉、后果最严重**的一个：

- **源节点必然中招**：`FfmpegDemuxerNode` / `FileSourceNode` 没有 `input_`，其唯一阻塞点就是 `output_->Send()`。停止时下游解码器先退出，从此无人再 `Receive()`，源节点永远等不到空位。此时 `input_->Abort()` 打在空指针上完全无效。
- **中间节点同样中招**：`VideoDecoderNode` 在下游 `AVSyncNode` 先停止后，若恰好卡在 `output_->Send()`，`input_->Abort()` 只能救"等收"，救不了"等发"。
- **表现为概率性死锁**：仅当"停止瞬间该管道恰好满"才触发。本地短视频队列压不满，测不出来；4K 高码率、弱网卡顿、或用户在缓冲中点停止时必现。症状是整个 UI 冻死、堆栈停在 `pthread_cond_wait` 看不出因果，属最难排查的一类缺陷。

因此 `INode::AbortLinks()` 作为**框架级默认行为**同时断开两端，不依赖任何派生类记得覆写 —— 与"epoch 过滤应下沉到框架"（见第 10 节风险 2）是同一条设计原则：**正确性不能建立在开发者记忆之上**。

### 为什么用 `Abort` 而非 `Close`

两者语义严格分工，不可混用：

| 动作 | 语义 | 服务场景 |
|------|------|---------|
| `Close()` | 禁止再 Send，**保留存量**供下游排空，排空后 `Receive` 返回 `nullopt` | EOF 正常收尾（数据要完整处理完） |
| `Abort()` | 立即唤醒收发两端，**丢弃存量**，Send/Receive 一律失败 | Stop / Seek（拆图丢弃一切） |

`Stop` 的语义就是"拆图丢弃一切"，与 `Abort` 完全对齐。需要完整收尾的节点（如 `Mp4MuxerNode` 要保证录制文件不截断）可覆写 `OnStopRequested()` 改用 `Close()` —— 这正是虚函数应有的用法：**默认安全，特例可调**。

> **`Abort` 不可逆**：`ThreadSafeQueue::aborted_` 一旦置真便无复位接口，被 `Abort` 过的 Link 永久失效。故 Stop 之后 Link 必须随 Pipeline 一同销毁、下次 `Open` 时重建，**不可复用同一条 Link 再次 Start**。这一约束限制了未来"Stop 后不拆图直接重 Start"的优化路径，需在做此类优化前先为队列补 `Reset` 语义。

### `AbortLinks` 与 `OnStopRequested` 为何拆成两个函数

- `OnStopRequested()` 回答**何时**断开 —— 生命周期钩子，由 `RequestStop()` 调用；
- `AbortLinks()` 回答**如何**断开 —— 具体动作，可被派生类复用。

拆开后，派生类获得第三种覆写姿势："先做默认动作，再补自己的"：

```cpp
// FfmpegDemuxerNode：一进双出，需额外处理自持的第二条输出管道
void OnStopRequested() override {
    AbortLinks();            // 复用框架默认行为，无须书写 INode<InT,OutT>:: 模板限定名
    audioOutput_->Abort();   // 再补自己那条
}
```

若不拆分，派生类想保留默认行为就得写 `INode<Packet, VideoFrame>::OnStopRequested()` —— 模板参数一多极易写错，且编译错误表现为大段模板展开，难以定位。

### 方案甲核心：停止不进全局态，通过注入判据让谓词能"看到"停止

**设计哲学**：
- `PipelineState` 只有 `{kRunning, kPaused}` 两态，**不包含 kStopped**——停止不是"播放状态"，是"线程生命周期终止"。
- 停止信号存于各 Node 的 `stopRequested_`（节点级标志），不污染共享的 SyncContext。
- `WaitWhilePaused` 的谓词需要"看到"停止，但 `stopRequested_` 在 Node、谓词在 SyncContext，怎么桥接？→ **Node 注入一个回调** `should_stop()` 给谓词用。

### 代码实现（三方协同）

#### SyncContext 侧

```cpp
/**
 * @brief 暂停时阻塞，直到恢复播放或收到停止信号
 * @param should_stop 停止判据回调，由调用方(Node)注入，返回 true 表示"该停了"
 * @details 谓词 = "不再暂停(state 变 kRunning)" || "该停了(should_stop()=true)"，
 *          两个条件任一为真即返回。这样 SetRunning(改 state)或 Pipeline::Stop
 *          (RequestStop 置标志 + WakeAllPausedThreads 唤醒)都能让线程出来。
 */
void WaitWhilePaused(const std::function<bool()>& should_stop) {
    std::unique_lock<std::mutex> lock(pauseMutex_);
    pauseCv_.wait(lock, [this, &should_stop] {
        return state_.load() != PipelineState::kPaused   // 恢复播放
            || should_stop();                             // 或停止
    });
}

/**
 * @brief 唤醒所有睡在暂停 CV 上的线程（纯 notify，不改任何状态）
 * @note 调用前**必须**先置好停止标志(RequestStop)，否则谓词 should_stop()
 *       仍为 false，线程被唤醒后复查谓词发现"既不是 Running 又没停"会再睡回去。
 */
void WakeAllPausedThreads() {
    pauseCv_.notify_all();
}
```

#### Node 循环侧

```cpp
void ThreadMain() final {
    while (true) {
        // ① 停止优先：节点级标志
        if (IsStopRequested()) break;

        // ② 暂停：注入自己的停止标志给谓词用
        if (sync_->GetState() == PipelineState::kPaused) {
            sync_->WaitWhilePaused([this] { return IsStopRequested(); });
            continue;   // 醒来回顶：若因停止醒来，下轮①就 break；若因 Resume，走③干活
        }

        // ③ 运行：干活
        if (!Tick()) break;
    }
}
```

#### Pipeline::Stop 侧（顺序是生死攸关的）

```cpp
void Pipeline::Stop() {
    // 【关键】顺序：先置标志，后唤醒（反了会死锁）

    // ① 先置所有节点的停止标志 + 断开两端管道（踹卡点 B 与 C）
    for (auto& node : nodes_) {
        // 置 stopRequested_ + OnStopRequested → AbortLinks()
        //   → input_->Abort()  唤醒"等收"的线程（卡点 B）
        //   → output_->Abort() 唤醒"等发"的线程（卡点 C，源节点唯一阻塞点）
        node->RequestStop();
    }

    // ② 再唤醒暂停 CV（踹卡点 A）
    //    此时谓词 should_stop() 已是 true，线程能真正返回
    sync_->WakeAllPausedThreads();

    // ③ join 所有线程
    for (auto& node : nodes_) {
        node->WaitForStop();
    }
}
```

### 为什么顺序是"RequestStop 在前，Wake 在后"？

**反例（先 Wake 后 RequestStop，会死锁）**：
```
WakeAllPausedThreads() → notify → wait 醒来复查谓词
  → should_stop()=false(还没 RequestStop) 且 state 仍是 kPaused
  → 谓词返回 false → 立刻又睡回去
随后 RequestStop() 置 stopRequested_ → 但线程已睡死，没人再 notify → join 挂死
```

**正例（先 RequestStop 后 Wake）**：
```
RequestStop() 置所有 stopRequested_=true
  → WakeAllPausedThreads() → notify → wait 醒来复查谓词
  → should_stop()=true → 谓词返回 true → 线程真返回
  → 回循环顶 IsStopRequested()=true → break → join 成功
```

### 对标 GStreamer

GStreamer 的 flush 事件 = 我们的 `Abort()`，用于踹醒阻塞在数据流(pad push/pull)上的线程。我们的方案甲与其一致：**停止由各 element(Node)自己的标志驱动，全局状态机只管播放/暂停**。GStreamer 的 `GST_STATE_NULL` 传播时也是逐 element 设置，而非一个全局 `kStopped` 让所有 element 去查。

---

## 9. 架构自检：SRP / RAII / DAG / 现代播放器

### 9.1 单一职责（SRP）核对

| 类 | 唯一职责 | 是否单一 |
|----|---------|---------|
| IPlayer / IVideoSink | 定义契约 | ✅ 纯接口 |
| PlayerFacade | UI 指令 ↔ Pipeline 操作的翻译 + 状态校验编排 | ✅（不碰线程/队列/解码） |
| FSM | 判定跃迁合法性 | ✅（只判不做） |
| Pipeline | 拓扑装配 + 启停编排 + 所有权容器 | ✅（不定义节点内部逻辑） |
| SyncContext | 全局态 + epoch 的共享存取 | ✅（不持节点、不处理数据） |
| INodeBase | 线程生命周期 | ✅（与数据类型无关） |
| INode | 强类型 Link + 标准循环骨架 | ✅（不定义单项处理） |
| 具体 Node | 实现一种数据变换 (Tick) | ✅ |
| Link | 语义化管道动词 | ✅（转发给队列） |
| ThreadSafeQueue | 线程安全有界队列 | ✅（不知 DAG 语义） |

**结论**：职责切分干净，每类改动原因单一。INodeBase/INode 两层拆分正是 SRP 的典范——"管线程"和"管数据类型"分离。

### 9.2 RAII 核对
- ✅ 所有堆对象由智能指针托管，零裸 `new`/`delete`（IVideoSink* 除外，但那是"借用非拥有"，由 UI 侧 RAII）。
- ✅ 线程被 INodeBase 析构兜底 join。
- ✅ Link/队列/SyncContext 随持有者析构自动回收，引用计数保证顺序安全。
- ✅ FFmpeg 裸资源（AVPacket/AVFrame/AVFormatContext）将在 D10+ 用自定义删除器/RAII 包装（Packet/VideoFrame DTO）。

### 9.3 DAG 解耦核对
- ✅ 节点只认 `input_`/`output_` 两条 Link，**不持有上下游节点指针**——真正的解耦，节点可独立测试（灌一个假 Link 即可）。
- ✅ 强类型 Link 把"接错线"变成编译期错误（`Link<Packet>` ≠ `Link<VideoFrame>`）。
- ✅ 控制流走 SyncContext 广播，**不需要节点间点对点握手**（epoch 自扩散）。

### 9.4 对标现代播放器
- ✅ 线程 + 有界队列 pull 模型 = FFmpeg/GStreamer 主流范式。
- ✅ 全局状态机 + flush = GStreamer element state + flush 事件。
- ✅ 音频主时钟 = ffplay/主流播放器的经典选择。
- ✅ epoch 世代号作废旧帧 = 无锁 seek 的现代做法（优于多标志握手）。

---

## 10. 仍存在的设计风险（诚实清单）

> 以下是当前设计里**尚未闭合**或**需要警惕**的点，按优先级排列。不解决不影响 D9，但要在对应 Day 前想清楚。

1. **【中】停止时的节点回收顺序**（风险等级由【高】下调）
   原担忧是"生产者阻塞在 Send 到已死消费者"需靠反向回收（先消费者后生产者）规避，而双输出 Demuxer + 汇聚 AVSync 的拓扑不是简单链，回收序难以精确定义。
   **`AbortLinks()` 落地后此问题已大幅弱化**：任何节点无论睡在"等收"还是"等发"，`RequestStop()` 都能立即唤醒它，不再依赖"下游必须比上游后死"这一时序前提。停止正确性因此**与回收顺序解耦**。
   → 仍建议 D16 按拓扑逆序 `join`（利于让下游先排空、减少无意义的丢弃），但这已是**优化项而非正确性前提**。死锁回归测试仍需保留。

2. **【高】epoch 过滤散落在每个节点，易漏写**
   "比较 epoch 丢弃旧帧"靠各节点 `Tick()` 自觉执行。任一节点漏写 → Seek 后闪旧画面。
   → 需在 INode 层或 DTO 层提供统一的 `ShouldDrop(item)` 辅助，降低漏写概率。**这与已闭合的风险 5 是同一类问题**（正确性依赖开发者记忆），而卡点 C 的处理方式（下沉为框架默认行为）已验证有效，此处应采用同一策略，最迟在 D11 第一个真实 Node 落地前完成。

3. **【中】Facade 状态 (PlaybackState) 与 SyncContext 状态 (PipelineState) 双份**
   UI 态和内核机械态是两套枚举，需保证不漂移（如 UI 显示 Playing 但 SyncContext 还是 Paused）。→ 明确"Facade 态是权威，SyncContext 态是其投影"，跃迁时同步更新，禁止各写各的。

4. **【中】IVideoSink 裸指针的解绑时序**
   UI 销毁 OpenGLView 前必须 `DetachDisplayView`，且要保证此刻没有节点线程正在 `OnReceiveVideoFrame` 中途。→ 推帧处加锁（已有 sinkMutex_ 先例）+ Detach 与推帧互斥；文档化"UI 必须先 Detach 再析构 Sink"的契约。

5. ~~**【中】背压 + 暂停的交互**~~ → **已闭合（D9 + D10 重构）**
   *原风险*：暂停时数据流冻结，若某 Link 恰好满，上游线程可能阻塞在 `Send()` 而非睡在暂停 CV，需确认所有阻塞点都被覆盖。
   *结论*：**当时的实现确实存在死锁**。原 `INode::OnStopRequested()` 只 `Abort()` 了 `input_`，而"等发"（卡点 C）无人唤醒 —— 源节点没有 `input_`，这一拳完全打空。已用两节点串联单测复现：改动前"源节点 + 暂停中队列满 + 停止"用例**进程永久挂死**（`join` 无返回）。
   *修复路径*：
   - D9: 尝试引入纯虚 `AbortLinks()`，要求所有派生类实现；
   - D10 重构: 发现纯虚 + 析构调用冲突（vtable 退化导致"pure virtual method called"崩溃）→ 改为**空默认实现**（方案 A），派生类按需覆写；`INode<InT,OutT>` 提供一进一出的标准实现，多路节点自行覆写；
   - D9 单测补测"源节点不覆写 + 队列满 + 停止"场景，验证基类空实现 + `INode` 标准实现可保底兜住卡点 C。
   *遗留*：卡点表已从"两个"更正为"三个"；D16 `Pipeline::Stop` 落地时须保留此回归用例。

6. **【低】Seek 期间在飞帧与 kFlush 带内信令的竞态**
   epoch 作废旧帧是"惰性丢弃"，但解码器内部可能已缓存 B 帧重排队列，需 `avcodec_flush_buffers` 配合。→ D17 seek 落地时处理带内 kFlush 与 epoch 的协同。

7. **【低】内存序目前统一 seq_cst**
   求稳但非最优。→ 功能稳定后可将 epoch/state 降到 acquire/release，需配基准测试证明收益，不可拍脑袋改。

---

## 11. 结论

架构在 SRP / RAII / DAG 解耦 / 现代播放器范式四个维度上**自洽且经得起推敲**，所有权模型清晰（unique 独占、shared 共享、裸指针借用三者边界分明）。第 10 节的风险均为"已知且有应对时点"的可控项，不构成架构级返工。

**方案选型：方案甲**（停止归 Node，注入停止判据）
- `PipelineState` 只有 `{kRunning, kPaused}` 两态，停止不进全局态。
- 停止信号由各节点的 `stopRequested_` 表达，通过注入回调 `should_stop()` 让 `WaitWhilePaused` 谓词能"看到"它。
- 循环只判 `IsStopRequested()`，不判全局 `kStopped`。
- Pipeline::Stop 顺序：① RequestStop 先置标志（内含 `AbortLinks` 断开两端），② WakeAllPausedThreads 再唤醒，③ WaitForStop join。
- 职责四层分离：Player 控制播放 / Pipeline 管 DAG / SyncContext 存共享态(state+epoch) / Node 管线程生命周期。
- 扩展性：未来多 Pipeline(Recording/Preview/Transcode)各自独立停止，不争抢同一全局 kStopped。

### D9 落地情况（已完成并通过验证）

1. ✅ `SyncContext` 砍成两态（移除 kStopped），添加 `WaitWhilePaused(should_stop)` 与 `WakeAllPausedThreads()`；
2. ✅ `INode::ThreadMain` 实现方案甲三态循环（声明为 `final`，只判 `IsStopRequested()`，`WaitWhilePaused` 注入 lambda）；
3. ✅ 两节点串联单测：正常流转（不丢不重不乱序）/ 暂停恢复（计数冻结、睡 CV 不空转）/ 停止 join 不挂死 / epoch 世代过滤，四类断言全部通过，TSan 零数据竞争；
4. ✅ **补测发现并修复卡点 C 死锁**：单测构造"暂停中队列满 → 停止"用例时暴露 `OnStopRequested` 只 Abort `input_` 的缺口，源节点必然挂死。引入 `INode::AbortLinks()` 后闭合（见第 8 节与第 10 节风险 5）。

> 这条是"为并发写单测"价值的直接体现：设计评审阶段第 10 节已把该场景列为【中】风险并写明"D9 单测需专门构造"，单测确实兑现了它 —— 若跳过此用例，缺陷会一直潜伏到 D11 `FfmpegDemuxerNode` 上线，届时表现为概率性 UI 冻死，排查成本高出一个量级。

**D11 前置条件已全部解除（D9 重构 + 卡点 C 闭合）：**
1. ~~`INode<InT,OutT>` 固定一进一出，而 Demuxer 是一进双出~~ → **已解决**：`ThreadMain` 上移至 `INodeBase`，多路节点直接继承基类并自己持有 Link 成员；
2. ~~卡点 C（输出队列满时"等发"的线程无人唤醒）导致停止挂死~~ → **已闭合**：`AbortLinks()` 改为虚函数默认空实现（方案 A），基类析构调用安全，派生类按需覆写；D9 单测已验证"源节点不覆写 + 暂停中队列满 + 停止"场景 join 成功；
3. `StreamInfo` DTO 尚不存在，Demuxer 探测出的流参数无处安放 → 需先补 `src/av_types/stream_info.h`；
4. 风险 2（epoch 过滤易漏写）应在第一个真实 Node 落地前下沉到框架，避免重蹈覆辙。

