/**
 * @file alsa_audio_sink
 * @brief 使用 Linux ALSA 接收 PCM 声音数据，完成播放、暂停、清空和尾音播放
 * @details
 * ALSA - Advanced Linux Sound Architecture 是 Linux 的音频系统，包含应用接口和内耗驱动
 * 这里通过 ALSA 库的 PCM 播放接口，把未压缩的声音数据交给设备或音频插件
 * 数据路径： AudioSinkNode -> Write -> ALSA 缓冲区 -> 设备和输出设备
 *
 * snd_pcm_t 是播放流的句柄：ALSA 用它记录当前设备、缓冲区和播放状态
 * 本对象独占这个句柄，各操作由音频节点的同一个工作线程依次调用
 * SND_PCM_NONBLOCK 表示写入或者排空时不等待设备，未写完成的数据由节点保留后重试
 *
 * shadow_ 影子缓冲区保存已接受但尚未确认播完的数据副本，供不支持设备暂停时恢复播放使用
 * physical_frames_ 是已写入 ALSA、尚未确认播放完的 前部长度，不是历史写入总量
 * snd_pcm_delay 根据设备报告估计待播放梳理，它用于估算进度，不能精确测量扬声器发声时刻
 *
 * 播放欠载（音频供给不及时）和设备挂起有恢复处理，其他错误通过异常提交给报告节点
 */

#include <alsa/asoundlib.h>

#include <algorithm>  ///< 标准算法库 std::min、std::max、std::clamp
#include <cerrno>
#include <cstddef>  ///< size_t
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "audio_decoder/iaudio_sink.h"
#include "audio_decoder/pcm_frame.h"
#include "mediacore/logger.h"

namespace mediacore {

namespace {
/**
 * @class AlsaAudioSink
 * @brief 实现 IAudioSink，把统一格式的声音数据写入 ALSA
 * @details
 * 主要职责：
 *   1. Open()：打开 PCM 句柄，并协商格式、period、buffer 和软件参数
 *   2. Write()：将新 PCM 帧提交给 ALSA，并保存实际接受部分的副本
 *   3. UpdatePlaybackProgress()：根据 ALSA 状态和 delay 估算已经完成的帧数
 *   4. PumpReplay()：把暂停或恢复后尚未重新提交的旧数据按顺序送回 ALSA
 *   5. Pause()/Flush()/Drain()：分别实现暂停、丢弃旧数据和播放完尾音
 * snd_pcm_open 创建句柄，本对象在析构时使用 snd_pcm_close 释放，影子数据由 vector 自动释放
 * 本类不创建线程也不加锁，调用方不能同时执行 Write / Pause / Flush 等操作
 */
class AlsaAudioSink final : public IAudioSink {
public:
    /**
     * @brief 构造AlsaAudioSink 对象，保存设备名称和请求的缓冲时长
     * @param device ALSA PCM 名称，如：default / plughw:0,0
     * @param buffer_ms 请求的缓冲区时长（毫秒）[5,2000]
     * @throw std::invalid_argument 设备名称为空或时长超出
     */
    AlsaAudioSink(std::string device, int buffer_ms) : device_(std::move(device)), buffer_ms_(buffer_ms) {
        // 注意：成员初始化列表先于函数体执行，形参 device 此时已被 move 走，
        // 状态是"有效但未指定"（std::string 上通常就是空串）。
        // 因此必须检查已经保存的成员 device_，检查形参会无条件判定为空并每次构造都抛异常。
        if (device_.empty() || buffer_ms_ < kMinBufferMs || buffer_ms_ > kMaxBufferMs) {
            throw std::invalid_argument("ALSA: device must be non-empty and buffer_ms must be [5,2000]");
        }
    }

    /**
     * @brief 析构函数：立即丢弃设备中尚未播放的声音并关闭 PCM 句柄
     */
    ~AlsaAudioSink() override { ClosePcmNoThrow(); }

    /**
     * @brief 禁止拷贝和移动
     */
    AlsaAudioSink(const AlsaAudioSink&) = delete;
    AlsaAudioSink& operator=(const AlsaAudioSink&) = delete;

    AlsaAudioSink(AlsaAudioSink&&) = delete;
    AlsaAudioSink&& operator=(AlsaAudioSink&&) = delete;

    /**
     * @brief 打开设备并配置相关参数
     */
    void Open() override {
        if (pcm_ != nullptr) {
            return;
        }

        // 重置本对象各个状态
        ResetPlaybackBookkkeeping();

        LOG_INFO("ALSA opening device '{}', requestd {} Hz, {} channels, S16, buffer {} ms",
                 device_,
                 PcmFrame::kSampleRate,
                 PcmFrame::kChannels,
                 buffer_ms_);

        snd_pcm_t* opened_pcm = nullptr;
        Check(snd_pcm_open(&opened_pcm, device_.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK), "open");

        // 只有 Open 成功后才把句柄交给本对象持有
        pcm_ = opened_pcm;
        try {
            ConfigurePcm();
        } catch (...) {
            // 不能让参数配置失败后的半初始化句柄留在对象里
            ClosePcmNoThrow();
            ResetPlaybackBookkkeeping();
            throw;
        }
    }

    /**
     * @brief 将新的音频帧提交给 ALSA
     * @param data 交错存放的立体声 S16 PCM 数据，frames > 0 时 必须有效
     * @param frames 本次希望提交的采样帧数，不是 int16_t 数值个数
     * @return ALSA 本次实际接受的帧数，0 表示当前不能接受新数据或请求为空
     * @details
     * 非阻塞的 writei 可能：
     * 1. 返回整数且 < frames：短写，只接受一部分
     * 2. 返回 -EAGAIN：当前暂时没有足够的可写空间
     * 3. 返回 -EPIPE/ESTRPIPE：分别尝试恢复欠载/挂起
     * 4. 返回其他负值：通过异常交给上册
     */
    std::size_t Write(const std::int16_t* data, std::size_t frames) override {
        RequireOpen();

        if (paused_ || frames == 0) {
            return 0;
        }

        if (data == nullptr) {
            throw std::invalid_argument("ALSA: null PCM data");
        }

        // 根据 ALSA 报告的播放延迟，删除 shadow_ 中估计已经播放完的数据
        RefreshShadowFromAlsa();

        // 尝试重送 shadow 中还未重新提交给 ALSA 的后缀 of the function
        PumpReplay();

        // 如果本次还没有完全写入，则继续重送，不允许接受新帧
        if (submitted_pending_frames_ < ShadowFrames()) {
            return 0;
        }

        // snd_pcm_uframes_t 是 ALSA 的无符号帧数类型，若调用方传入了超过该类型可表示范围的 size_t
        // 本次只提交类型可表达范围之内的部分，余下的数据由调用方继续处理
        const auto requested_frames = static_cast<snd_pcm_uframes_t>(
            std::min<std::size_t>(frames, static_cast<std::size_t>(std::numeric_limits<snd_pcm_uframes_t>::max())));

        if (requested_frames == 0) {
            return 0;
        }

        const snd_pcm_sframes_t written = snd_pcm_writei(pcm_, data, requested_frames);

        if (written < 0) {
            if (written != -EAGAIN) {
                Recover(static_cast<int>(written));
            }
            return 0;
        }

        const auto accepted_frames = static_cast<std::size_t>(written);
        const auto accepted_samples = accepted_frames * PcmFrame::kChannels;

        // vector 保存的是 int16_t 采样值，而 ALSA 的 writei 以帧为单位,所以复制范围需要把帧数乘以声道数
        shadow_samples_.insert(shadow_samples_.end(), data, data + accepted_samples);
        submitted_pending_frames_ += accepted_frames;
        return accepted_frames;
        ;
    }

    /**
     * @brief 返回本对象已经接受、尚未确认播放完成的总帧数
     * @details
     * 该接口会顺便推进旧数据的重送，避免软件暂停恢复后没有新输入时，待重送尾部永远得不到提交机会
     * 返回值包括：
     * 已经提交给 ALSA，但尚未确认播放完成的帧
     * 已经被本对象接受，但由于软件暂停/恢复播放时待重新提交的帧
     */
    std::int64_t DelayFrames() override {
        RequireOpen();

        if (!paused_) {
            RefreshShadowFromAlsa();
            PumpReplay();

            // 重送后可能刚刚向设备提交了新帧，再读取一次进度，让返回值尽可能反映调用结束时的状态
            RefreshShadowFromAlsa();
        } else {
            // 硬件暂停时也更新一次可观测的进度，正常情况下暂停后队列不再推进
            RefreshShadowFromAlsa();
        }

        return static_cast<std::int64_t>(ShadowFrames());
    }

    /**
     * @brief 请求播放完所有的尾部数据
     * @return true：本对象没有待播放的数据 / 已排空，false：尚未完成，调用方稍后重试
     * Drain 与 Flush 不同：Drain 保留尾音，Flush 立即丢弃尾音
     * 非阻塞 PCM 的 snd_pcm_drain() 可能返回 -EAGAIN，表示排空尚未完成
     */
    bool Drain() override {
        RequireOpen();

        if (paused_) {
            return false;
        }

        RefreshShadowFromAlsa();
        PumpReplay();
        RefreshShadowFromAlsa();

        // 如果副本还有尚未重新提交的后缀，本次先不调用 drain，必须先把旧数据送完
        if (submitted_pending_frames_ < ShadowFrames()) {
            return false;
        }

        if (ShadowFrames() == 0) {
            return true;
        }

        const snd_pcm_state_t state = snd_pcm_state(pcm_);
        if (state == SND_PCM_STATE_DISCONNECTED) {
            Check(-ENODEV, "device disconnected before drain");
        }

        // 如果尾部太短、尚未达到自动启动阈值，明确 start 后 drain，避免尾音一直排队
        if (state == SND_PCM_STATE_PREPARED) {
            Check(snd_pcm_start(pcm_), "start tail");
        }

        const int result = snd_pcm_drain(pcm_);
        if (result == -EAGAIN) {
            return false;
        }

        if (result < 0) {
            Recover(result);
            return false;
        }

        // drain 成功表示该 PCM 流的排队数据已经完成，清理副本与计数器，保留已配置句柄
        shadow_samples_.clear();
        submitted_pending_frames_ = 0;
        return true;
    }

    /**
     * @brief 丢弃旧的待播放音频并重新准备设备
     * @details
     * 常见于 Seek、停止后重置、切换音频流等需要放弃旧数据的场景，不限于 Seek
     * snd_pcm_drop() 只处理当前 PCM 流；它不会清理 shadow_samples_，也不会清除上游节点尚未提交的数据
     */
    void Flush() override {
        RequireOpen();

        LOG_DEBUG("ALSA device '{}' discarding {} pending frames for Flush", device_, ShadowFrames());

        Check(snd_pcm_drop(pcm_), "drop");
        Check(snd_pcm_prepare(pcm_), "prepare after drop");

        shadow_samples_.clear();
        submitted_pending_frames_ = 0;
        hardware_pause_active_ = false;
        paused_ = false;
        suspend_warning_logged_ = false;

        // hardware_pause_supported_ 是设备能力，不是当前状态，因此 Flush 后不必清零。
        LOG_DEBUG("ALSA device '{}' Flush completed; ready for new audio", device_);
    }

    /**
     * @brief 暂停或恢复播放
     * @details
     * 优先尝试硬件暂停，设备不支持或当前状态不允许硬件暂停时，使用软件暂停：
     * 先更新剩余帧估计量，再 drop/prepare 清空设备队列，
     * shadow_samples_ 保留未确认完成的数据，恢复后通过 PumpReplay() 按顺序重新提交
     * 软件暂停存在固有的时间窗口：查询 delay 和 drop 不是原子操作，设备可能在两者之间
     * 继续推进少量帧，因此依赖 delay 的软件暂停无法精确到实际 DAC 发声的最后一个采样点。
     */
    void Pause(bool pause_requested) override {
        // 确认已经打开 ALSA 设备
        RequireOpen();

        // 如果当前的 paused_ 和这次调用 Pause() 想要达到的状态相同就不需要重复操作
        if (paused_ == pause_requested) {
            return;
        }

        // 第一部分：请求暂停
        if (pause_requested) {
            RefreshShadowFromAlsa();

            // 查询当前 PCM 状态，决定是否尝试硬件暂停
            const snd_pcm_state_t state = snd_pcm_state(pcm_);
            if (state == SND_PCM_STATE_DISCONNECTED) {
                Check(-ENODEV, "device disconnrcted before pause");
            }

            // 只有同时满足 PCM 配置声明支持硬件暂停且当前设备状态为 RUNNING，正在运行 才尝试硬件暂停
            if (hardware_pause_supported_ && state == SND_PCM_STATE_RUNNING) {
                // 第二个参数为 1，表示请求暂停
                const int result = snd_pcm_pause(pcm_, 1);

                if (result == 0) {
                    hardware_pause_active_ = true;

                    // 暂停成功后再次检查播放进度，更新尚未确认播放完成的副本
                    RefreshShadowFromAlsa();

                    paused_ = true;

                    LOG_DEBUG("ALSA device '{}' paused by hardware, {} pending frames", device_, ShadowFrames());

                    // 硬件暂停成功，不需要再执行软件暂停的 drop/prepare
                    return;
                }

                // 硬件暂停失败：常见情况包括设备不支持 pause，或者当前 PCM 状态不允许暂停
                // 接下来尝试软件暂停，以便仍然可以实现暂停功能
                LOG_WARN(
                    "ALSA device '{}' hardware pause failed, code {} ({}); "
                    "trying software pause",
                    device_,
                    result,
                    snd_strerror(result));

                // -ENOSYS：当前 PCM 实现不支持该操作
                // -EINVAL：参数或当前状态不允许执行该操作
                if (result != -ENOSYS && result != -EINVAL) {
                    Recover(result);
                }
            } else if (state == SND_PCM_STATE_RUNNING) {
                // 设备正在运行，但是没有声明支持硬件暂停，因此只能用软件暂停
                LOG_DEBUG(
                    "ALSA device '{}' using software pause;"
                    "hardware pause unsupported",
                    device_);
            } else {
                // 当前没有处于 RUNNING 状态：例如 PCM 可能尚未开始播放，或者已经处于某个准备状态
                // 对这些状态，本实现不强行调用硬件 pause，而是统一使用软件暂停流程，确保后续可以清理设备队列
                LOG_DEBUG("ALSA device '{}' using software pause in state {}", device_, snd_pcm_state_name(state));
            }

            // 使用软件暂停：执行到这里说明硬件暂停失败，需要清理 ALSA 队列

            // 1. drop
            Check(snd_pcm_drop(pcm_), "drop for software pause");

            // 2. prepare：重新准备 PCM，使用设备回到可以接受后续数据的状态
            Check(snd_pcm_prepare(pcm_), "prepare for software pause");

            // drop 已经清空了设备队列，因此原来的已提交计数不能继续保留
            // shadow_samples_ 保存 100 帧；原先提交给 ALSA 的前 60 帧；drop 后，60帧 被清空
            // 此时 shadow_samples_ 仍保存需要保留的 100 帧，但设备里已经没有原先那 60 帧，所以将计数归零
            // 恢复时，PumpReplay() 会从 shadow_samples_ 开头重新提交这些数据
            submitted_pending_frames_ = 0;

            // 软件暂停已经清空设备队列，不再认为存在有效的硬件暂停状态
            hardware_pause_active_ = false;
            paused_ = true;

            // 本次软件暂停流程完成，重置挂起警告的去重标志
            suspend_warning_logged_ = false;

            LOG_DEBUG(
                "ALSA device '{}' software pause completed, "
                "preserving {} frames for replay",
                device_,
                ShadowFrames());

            return;
        }

        // 第二部分：请求恢复播放
        // 硬件暂停需要先调用 snd_pcm_pause(0)；软件暂停虽然没有硬件恢复动作，
        // 但同样必须清除 paused_，否则 Write()/PumpReplay() 会一直返回 0，
        // 音频节点无法继续消费 PCM，最终会把上游队列堵满。
        if (hardware_pause_active_) {
            const int result = snd_pcm_pause(pcm_, 0);

            if (result < 0) {
                // 硬件恢复失败，记录错误原因，并尝试通过 Recover() 处理
                // Recover() 可能成功恢复、推迟重试，或者抛出无法处理的错误

                LOG_WARN(
                    "ALSA device '{}' hardware resume failed, code {} ({}); "
                    "attempting recovery",
                    device_,
                    result,
                    snd_strerror(result));

                Recover(result);
            }

        } else if (!paused_) {
            // 已经处于播放状态，无需重复恢复。
            return;
        }

        // 硬件恢复成功，或软件暂停完成后，统一清除逻辑暂停标志。
        // 软件暂停时 snd_pcm_drop()/prepare() 已经把设备置于可写状态，
        // 后续 PumpReplay() 会把保留的 shadow 数据重新提交。
        hardware_pause_active_ = false;
        paused_ = false;

        // 恢复后先刷新设备状态和播放进度，再尝试重送旧数据。
        RefreshShadowFromAlsa();
        PumpReplay();

        // 记录恢复后的状态:
        // ShadowFrames() 是本对象保留的总帧数,submitted_pending_frames_ 是其中已重新提交或仍留在设备中的前部帧数；
        // 两者之差就是当前等待重送的后缀帧数。
        LOG_DEBUG(
            "ALSA device '{}' pause cleared, {} pending frames, "
            "{} frames awaiting replay",
            device_,
            ShadowFrames(),
            ShadowFrames() - submitted_pending_frames_);
    }

private:
    // 48 kHz 下，240 帧对应 5ms，它是每次 PumpReplay 的提交上限
    static constexpr std::size_t kMaxReplayFrames = 240;
    static constexpr int kMinBufferMs = 5;
    static constexpr int kMaxBufferMs = 2000;

public:
    /**
     * @brief 查询设备环形缓冲区当前还能立即接收多少采样帧
     * @return 设备真实空闲帧数；设备不在可写状态时返回 0
     * @details
     * 用 snd_pcm_avail_update() 而不是 snd_pcm_delay()：后者在 PulseAudio/PipeWire
     * 插件下会把服务器端缓冲也算进去，可能大于本对象实际提交的总量，
     * 把这种虚高的 delay 当成"设备占用"会让上层可用量恒为 0 而永久停写。
     * avail_update 反映的正是 writei 能否立即接受新数据的空间，语义与写入口径一致。
     */
    std::int64_t WritableFrames() override {
        RequireOpen();

        if (pcm_ == nullptr) {
            return 0;
        }

        // 设备处于 XRUN/SUSPENDED 等异常状态时 avail_update 会返回负的 errno：
        // 先走一次恢复，再重试；恢复失败则报告 0，由上层按"暂时写不进去"处理
        snd_pcm_sframes_t available = snd_pcm_avail_update(pcm_);
        if (available < 0) {
            Recover(static_cast<int>(available));
            available = snd_pcm_avail_update(pcm_);
        }

        if (available < 0) {
            return 0;
        }

        return static_cast<std::int64_t>(available);
    }

    /**
     * @brief 汇总 ALSA 侧的可观测状态，供上层在疑似卡死时打日志
     * @return 形如 "state=RUNNING shadow=9600 submitted=9600 delay=9600 paused=0 hw_pause=0" 的一行文本
     * @note 只做查询，不推进重送、不恢复设备；pcm_ 为空时返回 "closed"
     */
    std::string DescribeState() const override {
        if (pcm_ == nullptr) {
            return "closed";
        }

        snd_pcm_sframes_t device_delay = -1;
        const int delay_result = snd_pcm_delay(pcm_, &device_delay);

        std::string text = "state=";
        text += snd_pcm_state_name(snd_pcm_state(pcm_));
        text += " shadow=" + std::to_string(ShadowFrames());
        text += " submitted=" + std::to_string(submitted_pending_frames_);
        text += " device_delay=" + std::to_string(device_delay);
        if (delay_result < 0) {
            text += "(err " + std::to_string(delay_result) + ")";
        }
        text += " paused=" + std::to_string(paused_ ? 1 : 0);
        text += " hw_pause=" + std::to_string(hardware_pause_active_ ? 1 : 0);
        return text;
    }

private:

    /**
     * @brief 配置 PCM 的硬件参数、软件参数与起播阈值
     * @details
     * 调用前 pcm_ 必须已经成功打开，若本函数任意一步失败，Open 的 catch 分支则会关闭句柄
     */
    void ConfigurePcm() {
        // 1. 硬件参数（格式、采样率、period 和 buffer）
        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        Check(snd_pcm_hw_params_any(pcm_, hw), "hw_params_any");

        Check(snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "set_access");

        Check(snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16), "set_format");

        Check(snd_pcm_hw_params_set_channels(pcm_, hw, PcmFrame::kChannels), "set_channels");

        // negotiated_rate 协商采样率
        unsigned int negotiated_rate = PcmFrame::kSampleRate;
        Check(snd_pcm_hw_params_set_rate_near(pcm_, hw, &negotiated_rate, nullptr), "set_rate_near");

        // 上游 PcmFrame 和时间线都以 固定 48 kHz 为契约，因此不能接收 44.1 kHz
        if (negotiated_rate != PcmFrame::kSampleRate) {
            throw std::runtime_error("ALSA: device cannot provide 48000 Hz, use a plug device");
        }

        // 把用户请求的毫秒转换为帧数，该数值是请求目标，但是不一定就是设备的容量
        const snd_pcm_uframes_t requested_buffer_frames =
            static_cast<snd_pcm_uframes_t>(buffer_ms_) * negotiated_rate / 1000;
        if (requested_buffer_frames == 0) {
            throw std::runtime_error("ALSA: requested buffer size is zero frames");
        }

        // 先请求 period：不超过 240 帧，且不超过请求 buffer 的一半
        snd_pcm_uframes_t requested_period_frames =
            std::min<snd_pcm_uframes_t>(static_cast<snd_pcm_uframes_t>(kMaxReplayFrames), requested_buffer_frames / 2);
        Check(snd_pcm_hw_params_set_period_size_near(pcm_, hw, &requested_period_frames, nullptr),
              "set_period_size_near");

        // buffer 是总容量，near 表示请求尽量接近的目标值，ALSA 可能回写另一个实际值
        snd_pcm_uframes_t negotiated_buffer_frames = requested_buffer_frames;
        Check(snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &negotiated_buffer_frames), "set_buffer_size_near");

        // 适配选择出的硬件参数，ALSA 会更新 hw 参数对象以反映最终选中的配置
        Check(snd_pcm_hw_params(pcm_, hw), "hw_params");

        // 必须读取最终 period 与 buffer，而不是把 set_*_near 的请求/中间值当作最终的结果
        snd_pcm_uframes_t actual_buffer_frames = 0;
        Check(snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer_frames), "get_buffer_size");

        snd_pcm_uframes_t actual_period_frames = 0;
        int period_direction = 0;
        Check(snd_pcm_hw_params_get_period_size(hw, &actual_period_frames, &period_direction), "get_period_size");

        hardware_pause_supported_ = (snd_pcm_hw_params_can_pause(hw) != 0);

        if (actual_buffer_frames == 0 || actual_period_frames == 0) {
            throw std::runtime_error("ALSA: negotiated period or buffer size is zero");
        }

        // 2. 软件参数（起播阈值、可用空间阈值）
        snd_pcm_sw_params_t* sw = nullptr;
        snd_pcm_sw_params_alloca(&sw);
        Check(snd_pcm_sw_params_current(pcm_, sw), "sw_param_current");

        // 达到 start_threshold 后 ALSA 自动起播，取三者最小值，避免必须填满整个 buffer
        //  1) 实际 buffer 容量；2) 两个实际 period；3) 用户最初请求的 buffer 帧数。
        const snd_pcm_uframes_t start_threshold =
            std::min({actual_buffer_frames, actual_period_frames * 2, requested_buffer_frames});
        Check(snd_pcm_sw_params_set_start_threshold(pcm_, sw, start_threshold), "start_threshold");

        // avail_min 表示可用/空闲帧达到多少时，等待机制可以通知应用，它不是起播时阈值，当前类不使用
        // ALSA poll/wait，而是由上游周期性调用公共方法并执行非阻塞写入
        Check(snd_pcm_sw_params_set_avail_min(pcm_, sw, actual_period_frames), "avail_min");
        Check(snd_pcm_sw_params(pcm_, sw), "sw_params");

        // 让配置后的 PCM 进入 PREPARED 状态，此时可以写入 PCM 音频，但是不一定开始播放
        Check(snd_pcm_prepare(pcm_), "prepared");

        const double actual_buffer_ms = static_cast<double>(actual_buffer_frames) * 1000.0 / negotiated_rate;
        const double actual_period_ms = static_cast<double>(actual_period_frames) * 1000.0 / negotiated_rate;

        LOG_INFO(
            "ALSA device '{}' ready: rate={} Hz, channels={}, "
            "requested buffer={} ms/{} frames, actual buffer={} frames/{:.2f} ms, "
            "period={} frames/{:.2f} ms, hardware pause supported={}",
            device_,
            negotiated_rate,
            PcmFrame::kChannels,
            buffer_ms_,
            requested_buffer_frames,
            actual_buffer_frames,
            actual_buffer_ms,
            actual_period_frames,
            actual_period_ms,
            hardware_pause_supported_);

        // 某些构建配置会将日志宏编译为空；显式标记这两个派生值已被有意计算。
        (void)actual_buffer_ms;
        (void)actual_period_ms;
    }

    /**
     * @brief 确认 PCM 打开成功且句柄存在
     * @details 这里只验证对象持有句柄，不代表设备仍连接、状态允许任意 API 调用或音频正在播放
     */
    void RequireOpen() const {
        if (pcm_ == nullptr) {
            throw std::logic_error("ALSA: device is not open");
        }
    }

    /**
     * @brief 根据 ALSA 报告的播放延迟，删除 shadow_ 中估计已经播放完的数据
     * shadow_ 保存尚未确认播放完成的数据副本
     * submitted_pending_frames_ 表示其中已提交给 ALSA、但尚未确认播放完成的帧数
     * 本函数通过 snd_pcm_delay() 估算已经播放完成的帧数，并从 shadow_ 开头删除相应的数据
     * 本函数也会处理 XRUN、设备挂起和设备断开，因此不是纯查询函数
     * 尚待重送的数据由 PumpReplay() 负责重新提交。
     */
    void RefreshShadowFromAlsa() {
        const snd_pcm_state_t state = snd_pcm_state(pcm_);

        // 设备断开是独立的生命周期故障，不应当作普通 XRUN 无限重试
        if (state == SND_PCM_STATE_DISCONNECTED) {
            Check(-ENODEV, "PCM disconnected");
        }

        if (state == SND_PCM_STATE_SUSPENDED) {
            Recover(-ESTRPIPE);
            return;
        }

        if (state == SND_PCM_STATE_XRUN) {
            Recover(-EPIPE);
            return;
        }

        // 没有已提交但未确认完成的帧时，shadow 里可能只剩待重送的后缀
        // 此时 snd_pcm_delay() 不参与计算，避免把待重送数据错误的算作设备队列
        if (submitted_pending_frames_ == 0) {
            return;
        }

        // SETUP 表示流已配置但当前不在可运行状态。在此类中，明确执行 drop/软件暂停时
        // 会将提交计数归零，Drain 成功也会清空副本；剩余场景按排空完成处理已提交前缀。
        if (state == SND_PCM_STATE_SETUP) {
            RemoveSubmittedPrefix(submitted_pending_frames_);
            return;
        }

        snd_pcm_sframes_t device_delay_frames = 0;
        const int result = snd_pcm_delay(pcm_, &device_delay_frames);
        if (result < 0) {
            // 查询状态与查询 delay 不是原子操作，故即使前面看到正常状态，这里仍需处理错误。
            Recover(result);
            return;
        }

        // 某些设备/插件可能给出负 delay。此处把负值按 0 处理，防止转换为无符号数后变巨数；
        // 这只是本实现的防御策略，不意味着真实硬件延迟一定为 0。
        const auto remaining_device_frames =
            static_cast<std::size_t>(std::max<snd_pcm_sframes_t>(0, device_delay_frames));

        // 本对象只根据 delay 的减少量估算已完成帧数：
        // 例如本对象追踪 100 帧，设备报告剩余 60 帧，就移除副本最前面的 40 帧。
        // 如果设备报告值不小于追踪值，则不凭空增加副本，也不删除数据。
        if (remaining_device_frames < submitted_pending_frames_) {
            RemoveSubmittedPrefix(submitted_pending_frames_ - remaining_device_frames);
        }
    }

    /**
     * @brief 删除 shadow_samples_ 最前面的若干帧，并同步已提交前缀计数
     * @param frames 要删除的真实，实际删除量不会超过已提交的前缀长度
     */
    void RemoveSubmittedPrefix(std::size_t frames) {
        const std::size_t frames_to_remove = std::min(frames, submitted_pending_frames_);

        if (frames_to_remove == 0) {
            return;
        }

        const std::size_t samples_to_remove = frames_to_remove * PcmFrame::kChannels;
        shadow_samples_.erase(shadow_samples_.begin(),
                              shadow_samples_.begin() + static_cast<std::ptrdiff_t>(samples_to_remove));

        submitted_pending_frames_ -= frames_to_remove;
    }

    /**
     * @brief 尝试重送 shadow 中还未重新提交给 ALSA 的后缀
     * @details
     * 用途：软件暂停调用 snd_pcm_drop() 清空设备缓冲后，shadow_samples_ 保留了未播完的数据，
     * 恢复时通过本函数分批写回 ALSA，确保声音无缝续播。
     *
     * shadow_samples_ 布局为：
     *   [0, submitted_pending_frames_)               已提交、尚未确认完成的前缀
     *   [submitted_pending_frames_, ShadowFrames())   等待重送的后缀
     *
     * 指针按 "采样值" 偏移，因此帧偏移必须乘声道数，重送的数据副本本来就已经在 shadow 中
     * 不能再次 insert 也不能作为新接受的数据重复向上游报告
     *
     * 单次调用预算限制（重要）：
     * 为避免长时间占用工作线程（影响 Stop/Seek 响应），单次调用最多重送 kMaxReplayFramesPerCall 帧。
     * 若 shadow 中剩余帧数超过此值，本次只发送部分，剩余部分留给下次 DelayFrames() 继续。
     * 默认预算 960 帧 (4 × 240) ≈ 20 ms，即使设备空闲也不会一口气写满 200 ms 缓冲。
     *
     * 退出条件：
     * 1. submitted_pending_frames_ == ShadowFrames()：全部重送完成
     * 2. snd_pcm_writei 返回 -EAGAIN 或 0：设备暂时写不下
     * 3. snd_pcm_writei 返回其他负值：调用 Recover() 处理错误
     * 4. 达到单次调用预算上限：留待下次继续
     */
    void PumpReplay() {
        if (paused_) {
            return;
        }

        // 单次调用最多重送的帧数（48 kHz 下约 20 ms）
        // 避免一次性写满整段 shadow（默认 9600 帧/200 ms）导致 Stop/Seek 响应延迟过大
        constexpr std::size_t kMaxReplayFramesPerCall = 4 * kMaxReplayFrames;  // 960 帧
        std::size_t replayed_this_call = 0;

        while (submitted_pending_frames_ < ShadowFrames() && replayed_this_call < kMaxReplayFramesPerCall) {
            const std::size_t waiting = ShadowFrames() - submitted_pending_frames_;
            const auto chunk = static_cast<snd_pcm_uframes_t>(
                std::min<std::size_t>(kMaxReplayFrames, waiting));

            const std::size_t sample_offset = submitted_pending_frames_ * PcmFrame::kChannels;
            const std::int16_t* replay_data = shadow_samples_.data() + sample_offset;

            const snd_pcm_sframes_t written = snd_pcm_writei(pcm_, replay_data, chunk);

            if (written < 0) {
                if (written != -EAGAIN) {
                    Recover(static_cast<int>(written));
                }
                // 负 errno 不是写入帧数；设备暂时写不下，留到下一次调用重试
                return;
            }

            if (written == 0) {
                return;
            }

            submitted_pending_frames_ += static_cast<std::size_t>(written);
            replayed_this_call += static_cast<std::size_t>(written);
        }
    }

    /**
     * @brief 按照项目的明确策略恢复播放欠载或设备挂起
     * @details
     * -EPIPE：放弃已提交前缀（XRUN 后无法可靠知道每帧是否已经实际发声）prepare 设备，保留尚待重送的后缀
     * 此策略可能产生断音，但避免盲目重放整段已提交数据
     * -ESTRPIPE：先尝试 snd_pcm_resume，成功时希望保留已经写入 ALSA 但是还未播放完的数据
     * 若暂时返回 -EAGAIN，将恢复推迟后的调用，其他恢复失败则 prepare 并把副本整体转为待重送
     * 其他错误交由 Check() 转为异常
     */
    void Recover(int error) {
        // 1. 设备欠载
        if (error == -EPIPE) {
            LOG_WARN(
                "ALSA device '{}' underrun: discarding {} submitted pending frames;"
                "retaining {} frames waiting for replay",
                device_,
                submitted_pending_frames_,
                ShadowFrames() - submitted_pending_frames_);

            // XRUN（播放欠载）意味着设备把缓冲区里的数据播完才饿死：已提交的前缀帧确实发过声，
            // 因此这里只删除副本，不把它们计为"丢弃"。删除后 ShadowFrames() 只剩待重送后缀，
            // 上层算出的 played = next_frame_ - ShadowFrames() 正好等于真实播放位置。
            // 若在此处额外让上层回退 next_frame_，会凭空补出一段静音并把后续音频整体后移。
            RemoveSubmittedPrefix(submitted_pending_frames_);
            Check(snd_pcm_prepare(pcm_), "prepare after underrun");
            suspend_warning_logged_ = false;
            return;
        }

        // 2. 设备挂起
        if (error == -ESTRPIPE) {
            if (!suspend_warning_logged_) {
                LOG_WARN(
                    "ALSA device '{}' suspended; attempting resume with {} pending frames", device_, ShadowFrames());
                suspend_warning_logged_ = true;
            }

            const int resumed = snd_pcm_resume(pcm_);
            if (resumed == 0) {
                // 当前这次挂起过程中，还没有打印过警告，允许打印
                suspend_warning_logged_ = false;
                LOG_INFO("ALSA device '{}' resumed after suspension; device queue preserved", device_);

                return;
            }

            if (resumed == -EAGAIN) {
                // 设备仍未恢复；不在这里阻塞循环，以便上层继续处理停止等控制请求。
                return;
            }

            LOG_WARN(
                "ALSA device '{}' resume failed, code {} ({});"
                "prepare and scheduling retained audio for replay",
                device_,
                resumed,
                snd_strerror(resumed));

            Check(snd_pcm_prepare(pcm_), "prepare after suspend");

            // prepare 已重置设备队列，保留 shadow，后续由 PumpReplay 从头重新提交
            submitted_pending_frames_ = 0;
            suspend_warning_logged_ = false;
            return;
        }

        Check(error, "PCM I/O");
    }

    /**
     * @brief 将 ALSA 错误码转换成带设备名字和操作名称的异常
     * @details
     * 可重试的 -EAGAIN 和本类专门处理的 -EPIPE / -ESTRPIPE 已经在调用分支处理
     */
    void Check(int result, const char* operation) const {
        if (result >= 0) {
            return;
        }

        const std::string message = std::string("ALSA ") + operation + " [" + device_ + "] (" + std::to_string(result) +
            "): " + snd_strerror(result);

        // 日志失败不能阻止异常抛出
        try {
            LOG_DEBUG("{}", message);
        } catch (...) {
            // 忽略日志异常
        }

        // 无条件抛出：ALSA 的原始错误必须上报，不能被日志层的异常掩盖
        throw std::runtime_error(message);
    }

    /**
     * @brief 获取 shadow_samples_ 中保存的采样帧数
     *
     * shadow_samples_ 保存的是交错排列的 int16_t 采样值，每个采样帧包含 PcmFrame::kChannels 个采样值
     */
    std::size_t ShadowFrames() const { return shadow_samples_.size() / PcmFrame::kChannels; }

    /**
     * @brief 关闭 PCM 句柄，析构和 Open 失败时，清理路径使用
     * @note 先 drop 把设备从 RUNNING 拉回 SETUP，避免某些插件在 close 时尝试
     *       冲刷待播数据；随后 close 真正释放句柄与设备资源。
     */
    void ClosePcmNoThrow() noexcept {
        if (pcm_ == nullptr) {
            return;
        }

        (void)snd_pcm_drop(pcm_);
        (void)snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }

    /**
     * @brief 重置本对象的各个状态
     */
    void ResetPlaybackBookkkeeping() noexcept {
        shadow_samples_.clear();
        submitted_pending_frames_ = 0;
        hardware_pause_supported_ = false;
        hardware_pause_active_ = false;
        paused_ = false;
        suspend_warning_logged_ = false;
    }

private:
    std::string device_;  ///< ALSA PCM 设备名称，默认为 default
    int buffer_ms_;  ///< 请求的 PCM 总缓冲时长（毫秒）
    snd_pcm_t* pcm_{nullptr};  ///< 本节点的 ALSA PCM 句柄
    std::vector<std::int16_t> shadow_samples_;  ///< 保存所有尚未确认播放完成的帧副本
    std::size_t submitted_pending_frames_{0};  ///< shadow_samples_ 已经提交给 ALSA 但是尚未确认播放完的帧数

    bool hardware_pause_supported_{false};  ///< 当前 PCM 配置声明是否支持硬件暂停
    bool hardware_pause_active_{false};  ///< 本对象是否成功请求了硬件暂停
    bool paused_{false};  ///< 本对象的逻辑暂停状态
    bool suspend_warning_logged_{false};  ///< 当前挂起恢复过程是否已经打印过警告，防止刷屏
};
}  // namespace

/**
 * @brief 创建 ALSA 音频输出对象
 * @details
 * 这里只是创建一个 AlsaAudioSink 实例并保存配置，但是真正的 打开设备操作由后面的 Open() 完成
 */
std::unique_ptr<IAudioSink> CreateAlsaAudioSink(std::string device, int buffer_ms) {
    return std::make_unique<AlsaAudioSink>(std::move(device), buffer_ms);
}

}  // namespace mediacore
