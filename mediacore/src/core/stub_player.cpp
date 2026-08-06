/**
 * @file stub_player.cpp
 * @brief 在真实 DAG 解码管线尚未落地前,后台线程周期性合成渐变 YUV420P 帧，推给 IVideoSink
 *        打通 UI → IPlayer 接口 → IVideoSink → OpenGLView 渲染
 */

#include "stub_player.h"

#include <chrono>
#include <condition_variable>
#include <cstddef> /// std::ptrdiff_t
#include <cstdint>
#include <mutex>
#include <thread>

#include "mediacore/av_types/video_frame.h"
#include "mediacore/iplayer.h"
#include "mediacore/playback_state.h" 
#include "mediacore/ivideo_sink.h"

namespace mediacore {

/**
 * @brief 构造函数，仅初始化成员，不启动线程
 */
StubPlayer::StubPlayer() = default;

/**
 * @brief 析构函数
 */
StubPlayer::~StubPlayer() { Stop(); }

// ================= IPlayer：渲染出口注入 =================

/**
 * @brief 绑定视频帧接收端
 * @param sink 非拥有观察者指针
 */
void StubPlayer::AttachDisplayView(IVideoSink* sink) {
    /// 保护锁 后台线程可能在读 sink_ 推帧， UI 线程此刻写入需要互斥
    std::lock_guard<std::mutex> lock(sinkMutex_);
    sink_ = sink;
}

/**
 * @brief 解绑视频帧接收端
 * @param 之前绑定的接收器，调用后后台线程不再向其推帧
 */
void StubPlayer::DetachDisplayView(IVideoSink* sink) {
    std::lock_guard<std::mutex> lock(sinkMutex_);

    /// 仅当传入的是当前 sink 的时候才去注销
    if (sink_ == sink) {
        sink_ = nullptr;
    }
}

// ================= IPlayer：播放控制 =================
/**
 * @brief 打开媒体源（桩：仅记录 url 并跃迁到 kOpening → 就绪）
 * @param url 媒体地址（桩阶段不实际解析）
 */
void StubPlayer::Open(const std::string& url) {
    (void)url;
    ChangeState(PlaybackState::kOpening);
}

/**
 * @brief 开始 / 恢复播放：跃迁到 kPlaying，拉起（或唤醒）后台造帧线程
 */
void StubPlayer::Play() {
    // 用 CAS（compare_exchange_strong）把"判断能否进入某状态"与"真正跃迁"合并为一次原子操作：
    // 只有 CAS 成功的调用者才有资格初始化线程，并发的其余调用者 CAS 必然失败而出局，
    // 从根上消除"两次 Play 同时建线程"的竞态——业务决策全交给 state_，joinable 只做资源回收

    // 情况一：从暂停恢复。kPaused --CAS--> kPlaying：线程还活着、只是睡着，赢家仅唤醒它
    PlaybackState expected = PlaybackState::kPaused;
    if (state_.compare_exchange_strong(expected, PlaybackState::kPlaying)) {
        stateCv_.notify_all(); // 唤醒挂起在 stateCv_ 上的 RenderLoop
        return;
    }

    // 情况二：首播 / 停止后重播逐个尝试从 kIdle / kOpening / kStopped 跃迁到 kPlaying，
    //         只有唯一赢家 CAS 成功，独占后台线程的初始化，无需额外加锁
    for (PlaybackState from : {PlaybackState::kIdle, PlaybackState::kOpening, PlaybackState::kStopped}) {
        expected = from;
        if (state_.compare_exchange_strong(expected, PlaybackState::kPlaying)) {
            // 回收上一轮 Stop() 已退出但句柄尚存的线程，确保对 joinable 对象赋值前先清空
            // （对 joinable 的 std::thread 赋值会触发 std::terminate）
            if (renderThread_.joinable()) {
                renderThread_.join();
            }
            abort_.store(false);
            renderThread_ = std::thread(&StubPlayer::RenderLoop, this);
            return;
        }
    }

    // 情况三：所有 CAS 均失败 —— 当前已是 kPlaying（或正被他人跃迁），幂等返回
}

/**
 * @brief 暂停：跃迁到 kPaused，后台线程在同步点挂起（不退出）
 */
void StubPlayer::Pause() {
    // 暂停只应从 kPlaying 发生。用 CAS 收紧：非播放态（kStopped/kIdle 等）忽略，
    // 避免 kStopped --> kPaused 这类非法跃迁，与 Play() 的 CAS 分派对称。
    PlaybackState expected = PlaybackState::kPlaying;
    if (state_.compare_exchange_strong(expected, PlaybackState::kPaused)) {
        // 状态已落到 kPaused；RenderLoop 下一循环在 wait 谓词处自然挂起（不退出、不占 CPU）
        stateCv_.notify_all();
    }
}
/**
 * @brief 停止：跃迁到 kStopped，两阶段停止后台线程并 join
 */
void StubPlayer::Stop() {
    // 阶段一：下发信令并唤醒睡眠线程
    abort_.store(true);
    ChangeState(PlaybackState::kStopped);
    stateCv_.notify_all();

    // 阶段二：回收线程
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
}

/**
 * @brief 跳转（桩：仅打日志占位，不涉及真实时间轴）
 * @param timestampUs 目标时间戳（微秒）
 */
void StubPlayer::Seek(int64_t timestampUs) { (void)timestampUs; }

// ================= IPlayer：状态查询 =================

/**
 * @brief 查询当前播放状态
 * @return 原子读取的 PlaybackState
 */
PlaybackState StubPlayer::GetState() const { return state_.load(); }

// ================= 内部逻辑 =================

namespace {

/**
 * @brief 合成一帧渐变测试图（YUV420P）
 * @param frameIndex 帧序号，驱动色彩随时间平移，产生"动"的观感
 * @return 持有一块紧凑 YUV420P 内存的 VideoFrame（shared_ptr 共享所有权）
 * @note Y 平面填水平渐变 + 帧号偏移；U/V 填随帧号缓慢变化的常量，使画面色调周期性流动
 *       av_mallocz 已将内存清零，故仅需写入即可，无需额外初始化
 */
std::shared_ptr<VideoFrame> GenerateFrame(uint64_t frameIndex, FrameSize size) {
    auto frame = std::make_shared<VideoFrame>(size);

    const uint8_t kOffset = static_cast<uint8_t>(frameIndex & 0xFF); // 逐帧平移量

    // --- Y 平面：水平渐变，叠加帧号偏移，形成横向流动的亮度条纹 ---
    // 行偏移用 ptrdiff_t 计算，避免 int 乘法在大分辨率下溢出后再隐式加宽为指针偏移
    for (int row = 0; row < size.height_; ++row) {
        uint8_t* line = frame->data_[0] + static_cast<std::ptrdiff_t>(row) * frame->linesize_[0];
        for (int col = 0; col < size.width_; ++col) {
            line[col] = static_cast<uint8_t>((col + kOffset) & 0xFF);
        }
    }

    // --- U / V 平面：整平面填随帧号缓慢变化的色度常量，画面色调周期性偏移 ---
    const int kUvWidth = (size.width_ + 1) / 2;
    const int kUvHeight = (size.height_ + 1) / 2;
    const uint8_t kU = static_cast<uint8_t>(128 + (kOffset / 2));
    const uint8_t kV = static_cast<uint8_t>(128 - (kOffset / 2));
    for (int row = 0; row < kUvHeight; ++row) {
        uint8_t* uLine = frame->data_[1] + static_cast<std::ptrdiff_t>(row) * frame->linesize_[1];
        uint8_t* vLine = frame->data_[2] + static_cast<std::ptrdiff_t>(row) * frame->linesize_[2];
        for (int col = 0; col < kUvWidth; ++col) {
            uLine[col] = kU;
            vLine[col] = kV;
        }
    }

    return frame;
}

} // namespace

/**
 * @brief 后台造帧循环（运行在 renderThread_ 上）
 * @details 三态调度：kPlaying 造帧推帧、kPaused 挂起等待、kStopped 退出
 *          每帧调用 GenerateFrame 合成渐变，经 mutex_ 取 sink_ 后推送，再按帧率休眠
 * @note 用 condition_variable 等待，避免暂停态忙等空转 CPU
 */
void StubPlayer::RenderLoop() {
    // 每帧目标间隔（毫秒）：由帧率换算，控制推帧节奏
    const auto kFrameInterval = std::chrono::milliseconds(1000 / kFps);

    while (true) {
        // --- 同步点：非播放态则睡眠，直到 Play() 唤醒或 Stop() 置 abort_ ---
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            // condition_variable::wait(lock, pred)：pred 为假时释放锁并挂起线程（0 CPU），
            // 被 notify 唤醒后重新持锁并复查 pred，天然免疫虚假唤醒
            stateCv_.wait(lock, [this] { return abort_.load() || state_.load() == PlaybackState::kPlaying; });
        }

        // 唤醒后优先响应退出信令
        if (abort_.load()) {
            break;
        }

        // 合成一帧（在后台线程分配，shared_ptr 托管，推给 sink 后本地引用释放）
        auto frame = GenerateFrame(frameIndex_++, FrameSize{kFrameWidth, kFrameHeight});

        // --- 推帧：锁内取 sink_，防与 Detach 竞态；sink 非空才投递 ---
        {
            std::lock_guard<std::mutex> lock(sinkMutex_);
            if (sink_ != nullptr) {
                sink_->OnReceiveVideoFrame(frame);
            }
        }

        // 按帧率休眠，模拟真实播放节奏
        std::this_thread::sleep_for(kFrameInterval);
    }
}

/**
 * @brief 内部状态跃迁（原子写 + 唤醒等待线程）
 * @param newState 目标状态
 */
void StubPlayer::ChangeState(PlaybackState newState) {
    // 原子写状态后广播，唤醒 RenderLoop 重新评估 wait 谓词
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_.store(newState);
    }
    stateCv_.notify_all();
}

} // namespace mediacore
