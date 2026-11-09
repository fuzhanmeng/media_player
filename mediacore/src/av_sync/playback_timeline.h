/**
 * @file playback_timeline.h
 * @brief 共享的媒体播放进度，协调音频、视频和播放控制
 * @details
 * PlaybackTimeline 是播放器各个节点共用的媒体时间轴
 * 只记录 "媒体应该播放到哪里"，并把这个位置提供给音频输出、视频显示和控制层
 * 有音频时，以音频作为主时钟，进行音画同步
 * 没有音频时，使用系统的稳定时钟进行计时以控制视频的播放进度，这里的系统时钟不是指的实际时间
 * Seek 时，时间轴会等待实际存在的音频和视频分支准备好跳转位置，并在所有分支到达 EOF 后确认媒体结束
 */

#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace mediacore {

/**
 * @brief 媒体中的音频分支或视频分支
 * @details
 * 媒体流轨道标识（采用二进制位组合，便于多轨聚合判定）： 1 表示只有音频， 2 表示只有视频，3 表示音视频都有
 */
enum class PlaybackTrack : std::uint8_t {
    kVideo = 1,  ///< 视频轨（第 1 位，对应二进制 01）
    kAudio = 2  ///< 音频轨（第 2 位，对应二进制 10）
};

/**
 * @class PlaybackTimeline
 * @brief  保存播放状态和播放位置，并在多条轨道之间协调跳转与结束
 * @details
 * 时间轴只保存和计算播放状态，不负责解码、启停设备或者更新 Pipeline，可以被多个线程共享
 */
class PlaybackTimeline {
public:
    // Diagnostic classification only; clock selection still uses AudioMasterLocked().
    enum class AudioClockReason : std::uint8_t {
        kAudioMaster,
        kNoAudioTrack,
        kNoAudioPosition,
        kAudioEos,
        kProgressTimeout
    };

    static const char* AudioClockReasonName(AudioClockReason reason) noexcept {
        switch (reason) {
            case AudioClockReason::kAudioMaster: return "AUDIO_MASTER";
            case AudioClockReason::kNoAudioTrack: return "NO_AUDIO_TRACK";
            case AudioClockReason::kNoAudioPosition: return "NO_AUDIO_POSITION";
            case AudioClockReason::kAudioEos: return "AUDIO_EOS";
            case AudioClockReason::kProgressTimeout: return "PROGRESS_TIMEOUT";
        }
        return "UNKNOWN";
    }

    /**
     * @struct Snapshot
     * @brief 某一瞬间播放状态与时间位置的完整只读快照
     */
    struct Snapshot {
        std::uint64_t epoch_{0};  ///< 播放世代号
        std::uint64_t state_version_{0};  ///< 控制更新次数：每次播放、暂停、跳转或停止都会加
        bool playing_{false};  ///< 是否正在播放
        bool seeking_{false};  ///< 是否正在等待跳转后各轨道到达目标位置
        bool stopped_{false};  ///< 是否已停止
        bool has_audio_position_{false};  ///< 当前版本是否收到过有效的音频进度
        bool is_audio_master_{false};  ///< 当前是否正以音频进度作为全局主时钟
        AudioClockReason audio_clock_reason_{AudioClockReason::kNoAudioPosition};
        std::int64_t audio_progress_age_ms_{-1};  ///< -1 means no position received
        std::int64_t audio_stall_timeout_ms_{0};
        std::int64_t seek_target_us_{0};  ///< 最近一次跳转的目标位置（微秒）
        std::int64_t media_position_us_{0};  ///< 当前估算的播放位置（微秒）
    };

    /**
     * @struct SeekResult
     * @brief 各输出分支完成跳转定位后的聚合汇总结果
     */
    struct SeekResult {
        std::int64_t position_us_{0};  ///< 最终到达的位置（微秒）：有视频轨道时取视频的位置，否则取音频的位置
        bool all_eof_{false};  ///< 是否所有轨道在跳到目标位置后都已到达结尾，没有数据可播
    };

    /**
     * @brief 创建对象时登记媒体包含哪些轨道，并设置音频卡顿的判定时长
     * @param has_video_track 媒体是否包含视频分支
     * @param has_audio_track 媒体是否包含音频分支
     * @param audio_stall_timeout 超过此时长后仍无音频进展，改为按照系统计时,默认 500 毫秒
     */
    PlaybackTimeline(bool has_video_track,
                     bool has_audio_track,
                     std::chrono::milliseconds audio_stall_timeout = std::chrono::milliseconds(500))
        : media_tracks_((has_video_track ? 1u : 0u) | (has_audio_track ? 2u : 0u)),
          has_audio_track_(has_audio_track),
          audio_stall_timeout_(audio_stall_timeout) {}

    /**
     * @brief 析构函数
     */
    ~PlaybackTimeline() = default;

    /**
     * @brief 同一个对象由音频、视频和控制模块共用，因此禁止复制和移动避免各模块持有互不相同的位置状态
     */
    PlaybackTimeline(const PlaybackTimeline&) = delete;
    PlaybackTimeline& operator=(const PlaybackTimeline&) = delete;

    PlaybackTimeline(PlaybackTimeline&&) = delete;
    PlaybackTimeline& operator=(PlaybackTimeline&&) = delete;

    /**
     * @brief 返回当前播放状态的副本，其中包含此刻估算的播放位置
     */
    [[nodiscard]] Snapshot Get() const {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 复制已记录的播放状态
        auto result = playback_state_;

        // 2. 在当前状态下计算位置，并填入音频时钟的相关信息
        result.media_position_us_ = PositionLocked();
        result.has_audio_position_ = has_audio_position_;
        result.is_audio_master_ = AudioMasterLocked();
        result.audio_clock_reason_ = result.is_audio_master_ ? AudioClockReason::kAudioMaster :
            !has_audio_track_ ? AudioClockReason::kNoAudioTrack :
            audio_finished_ ? AudioClockReason::kAudioEos :
            !has_audio_position_ ? AudioClockReason::kNoAudioPosition : AudioClockReason::kProgressTimeout;
        result.audio_progress_age_ms_ = has_audio_position_
            ? std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - last_audio_progress_time_).count()
            : -1;
        result.audio_stall_timeout_ms_ = audio_stall_timeout_.count();

        return result;
    }

    /**
     * @brief 启动跳转（Seek）流程
     * @details 由 Player 发起：音频和视频的清理由各个节点完成
     * @param epoch 本次跳转使用的版本号
     * @param seek_target_us 跳转目标时间位置
     */
    void BeginSeek(std::uint64_t epoch, std::int64_t seek_target_us) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 1. 记录新的轮次编号和目标位置，进入跳转等待状态，位置暂时冻结
        playback_state_.epoch_ = epoch;
        playback_state_.seek_target_us_ = seek_target_us;
        playback_state_.seeking_ = true;
        playback_state_.playing_ = false;

        // 2. 清空上一轮记录的“已到达轨道”和“已结束轨道”
        seek_ready_tracks_ = 0;
        finished_tracks_ = 0;
        seek_end_tracks_ = 0;
        seek_reported_ = false;

        // 3. 位置直接设为目标值，同时重置音频时钟状态和计时起点，避免把跳转前的音频位置误认为跳转后的位置
        aligned_seek_time_us_ = seek_target_us;
        has_audio_position_ = false;
        audio_finished_ = false;
        last_audio_position_us_ = seek_target_us;
        last_audio_progress_time_ = SteadyClock::now();
        base_media_time_us_ = seek_target_us;
        base_system_time_ = SteadyClock::now();

        // 4. 记录一次控制更新，并通知正在等待的模块
        ChangedLocked();
    }

    /**
     * @brief 切换播放和暂停状态
     * @param playing = true 表示播放， = false 表示暂停
     * @details
     * 由 Player 在用户按下播放或暂停时调用：切换前先记录当前播放位置作为新的计时起点
     * 暂停期间位置不变，恢复播放后从该位置继续，暂停的时长不计入播放进度
     */
    void SetPlaying(bool playing) {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 在切换状态之前，记下当前播放位置，作为新的计时起点。
        //    不能直接使用 Seek 目标：普通播放中的暂停可能已经推进了一段距离。
        base_media_time_us_ = PositionLocked();

        // 2. 记录当前的时钟读数，并用它作为恢复播放后的计时起点
        base_system_time_ = SteadyClock::now();
        playback_state_.playing_ = playing;

        // 3. 重置音频卡顿的判定时间，避免把恢复播放后重新出声的一小段时间误判为卡顿
        last_audio_progress_time_ = SteadyClock::now();

        // 4. 记录一次控制更新，并通知正在等待的模块
        ChangedLocked();
    }

    /**
     * @brief 结束跳转等待，按 Player 的决定继续播放或者保持暂停
     * @param playing true 表示跳转后继续播放，false 表示保持暂停
     */
    void CompleteSeek(bool playing) {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 退出跳转等待状态，设置为指定的播放状态
        playback_state_.seeking_ = false;
        playback_state_.playing_ = playing;

        // 2. 以当前时刻作为新的计时起点，跳转等待的时间不入播放进度
        base_system_time_ = SteadyClock::now();
        last_audio_progress_time_ = base_system_time_;

        // 3. 记录一次控制更新，并通知正在等待的模块
        ChangedLocked();
    }

    /**
     * @brief 记录某条轨道已经到达跳转目标位置，并在所有轨道都达到后返回结果
     * @param track 报告到达的轨道
     * @param epoch 播放版本号
     * @param position_us 该轨道实际到达的位置
     * @param eof 该轨道是否直接 Seek 到了结尾
     * @return 仅当媒体中所有的实际都 Seek 成功 时返回 SeekResult，其余情况返回 std::nullopt
     * @details 只有音频/视频时，对应的轨道 Seek 完成时返回结果即可
     */
    std::optional<SeekResult> MarkReady(PlaybackTrack track, std::uint64_t epoch, std::int64_t position_us, bool eof) {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 如果不属于本轮跳转的报告，或者当前不在跳转状态时，直接忽略
        if (epoch != playback_state_.epoch_ || !playback_state_.seeking_) {
            return std::nullopt;
        }

        // 2. 在已到达轨道中记录该轨道
        const auto bit = static_cast<unsigned>(track);
        seek_ready_tracks_ |= bit;

        if (eof) {
            // 同时记录该轨道是直接 Seek 到结尾的
            seek_end_tracks_ |= bit;
        }

        // 3. 确定最终的 Seek 位置：有视频轨道时取视频的 Seek 位置，没有时使用音频的 Seek 位置
        if (track == PlaybackTrack::kVideo || !(media_tracks_ & static_cast<unsigned>(PlaybackTrack::kVideo))) {
            aligned_seek_time_us_ = position_us;
        }

        // 4. 仍有轨道未到达，或本次结果已经返回过时，不再返回
        if (seek_ready_tracks_ != media_tracks_ || seek_reported_) {
            return std::nullopt;
        }

        // 5. 所有轨道都已到达：标记结果已返回，由最后到达的一条轨道返回结果
        seek_reported_ = true;
        return SeekResult{aligned_seek_time_us_, seek_end_tracks_ == media_tracks_};
    }

    /**
     * @brief 查询某条轨道本轮是否已经完成 Seek 预滚，完成 Seek 后仍保留结果直到下一轮
     * @details
     * Demuxer 用此状态结束单轨预滚，再重定位读取另一条轨道；不会改变就绪屏障
     */
    [[nodiscard]] bool IsTrackReady(PlaybackTrack track, std::uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return epoch == playback_state_.epoch_ && (seek_ready_tracks_ & static_cast<unsigned>(track));
    }

    /**
     * @brief 接收音频输出端报告的播放位置，并以此更新播放进度
     * @param epoch 播放版本号
     * @param position_us 音频实际播放到的位置
     * @details
     * 音频位置正常前进时，播放位置以音频为准，音频长时间未更新时，改用系统的单调时钟推算出的位置
     */
    void PublishAudio(std::uint64_t epoch, std::int64_t position_us) {
        // 音频节点与 GUI 查询、控制线程可能并发访问时间轴；所有状态字段都由
        // state_mutex_ 保护，先加锁再检查本轮 epoch 和播放状态。
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 忽略上一轮迟到的报告，以及跳转中、暂停中、已停止时的报告
        if (epoch != playback_state_.epoch_ || playback_state_.seeking_ || !playback_state_.playing_ ||
            playback_state_.stopped_ || audio_finished_) {
            return;
        }

        // 2. 记录更新前的位置，它可能来自音频，也可能来自单调时钟
        const auto current = PositionLocked();

        // 3. 判断音频时钟是否在推进
        const bool progressed = !has_audio_position_ || position_us > last_audio_position_us_;

        // 4. 判断在此之前音频是否仍可作为计时依据
        const bool audio_was_available = AudioMasterLocked();

        if (progressed) {
            // 音频位置前进了：记录新位置和当前时刻，恢复以音频位置为准
            last_audio_position_us_ = position_us;
            last_audio_progress_time_ = SteadyClock::now();
        }

        // 5. 确定新的计时起点
        // 音频位置在前进或仍在音频卡顿判断时长内：以音频时钟为准
        // 音频不在前进或卡顿超时：保留单调时钟推算出的位置，避免画面回退
        base_media_time_us_ =
            (progressed | audio_was_available) ? std::max(playback_state_.seek_target_us_, position_us) : current;

        base_system_time_ = SteadyClock::now();

        // 6. 标记已收到音频位置，并通知正在等待进度前进的模块
        has_audio_position_ = true;
        state_changed_cv_.notify_all();
    }

    /**
     * @brief 记录一条轨道已播放结束
     * @param track 调用方的轨道：音频输出节点传 kAudio，视频输出节点传 kVideo
     * @param epoch 本次 Seek 的版本号
     * @return 所有轨道都 Seek 结束时返回 true，重复 Seek、上一轮迟到的 Seek 或 已停止时返回false
     */
    bool MarkEnded(PlaybackTrack track, std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 编号不一致或已停止时，忽略本次调用
        if (epoch != playback_state_.epoch_ || playback_state_.stopped_) {
            return false;
        }

        // 2. 该轨道已经调用过本方法时，不重复记录
        const auto bit = static_cast<unsigned>(track);
        if (finished_tracks_ & bit) {
            return false;
        }

        // 3. 记录该轨道已经结束
        finished_tracks_ |= bit;

        // 4. 音频结束时，冻结当前位置并改用系统的单调时钟推进，使得剩余的视频画面可以继续播放结束
        if (track == PlaybackTrack::kAudio) {
            base_media_time_us_ = PositionLocked();
            base_system_time_ = SteadyClock::now();
            audio_finished_ = true;
        }

        state_changed_cv_.notify_all();

        // 5. 所有实际存在的轨道都结束后，才认为媒体播放结束
        return finished_tracks_ == media_tracks_;
    }

    /**
     * @brief 判断所有实际存在的轨道是否都已经结束
     */
    [[nodiscard]] bool AtEnd() const {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 媒体至少有一条轨道，且已结束的轨道与媒体包含的轨道完全一致
        return media_tracks_ != 0 && (finished_tracks_ == media_tracks_);
    }

    /**
     * @brief 停止播放：将当前位置固定下来并标记为停止，同时通知正在等待的模块
     * @details 由 Player 再关闭播放时使用
     */
    void Stop() {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 1. 记录停止时的位置，并将状态设置为停止
        base_media_time_us_ = PositionLocked();
        playback_state_.playing_ = false;
        playback_state_.stopped_ = true;

        // 2. 通知所有正在等待的模块，使其检查到停止状态后退出
        ChangedLocked();
    }

    /**
     * @brief 当前没有必要继续执行时，暂时让出线程，等待播放时控制发生了变化，或等待指定的最长时间后返回
     * @param state_version 调用方上次从 Get() 处读到的控制更新次数
     * @param duration 最长等待时间，默认 5 毫秒
     * @details
     */
    void Wait(std::uint64_t state_version, std::chrono::milliseconds duration = std::chrono::milliseconds(5)) {
        std::unique_lock<std::mutex> lock(state_mutex_);

        // 控制更新次数改变、对象已停止或等待超时时被唤醒
        state_changed_cv_.wait_for(lock, duration, [&]() {
            return playback_state_.state_version_ != state_version || playback_state_.stopped_;
        });
    }

    /**
     * @brief 等待播放位置到达目标时间，或控制状态发生变化
     * @param state_version 调用方上次从 Get() 处读到的控制更新次数
     * @param target_us 目标播放位置（微秒）
     * @param duration 最长等待时间，默认 5 毫秒
     * @details
     * 视频节点用它替代"反复 Get() 重算 delta"的自旋：位置由 PublishAudio（音频）或
     * 系统时钟外推推进，两者都会在状态变化时通知条件变量；纯视频场景下由 duration 兜底
     */
    void WaitForPosition(std::uint64_t state_version,
                         std::int64_t target_us,
                         std::chrono::milliseconds duration = std::chrono::milliseconds(5)) {
        std::unique_lock<std::mutex> lock(state_mutex_);

        state_changed_cv_.wait_for(lock, duration, [&]() {
            return playback_state_.state_version_ != state_version || playback_state_.stopped_ ||
                PositionLocked() >= target_us;
        });
    }

    /**
     * @brief 无锁读取控制版本号
     * @note 仅用于快速判断"是否需要进入完整的 Get()"，不保证与其他字段一致
     */
    std::uint64_t GetStateVersion() const noexcept { return state_version_atomic_.load(std::memory_order_acquire); }


private:
    // 一个单调时钟，只测量经过的时间，不受修改系统日期和时间的影响，不是现实时间
    using SteadyClock = std::chrono::steady_clock;

    /**
     * @brief 计算当前播放位置（微秒）
     */
    std::int64_t PositionLocked() const {
        // 1. 暂停、跳转等待或已停止时，位置保持不变
        if (!playback_state_.playing_ || playback_state_.seeking_ || playback_state_.stopped_) {
            return base_media_time_us_;
        }

        // 2. 音频位置可以作为主时钟时，直接返回以音频位置更新的计时起点
        if (AudioMasterLocked()) {
            return base_media_time_us_;
        }

        // 3. 如果有音频进度，但是音频超时不更新了，先判断超时时长，再使用单调时钟继续推进
        if (has_audio_track_ && has_audio_position_ && !audio_finished_) {
            const auto elapsed =
                SteadyClock::now() - std::max(base_system_time_, last_audio_progress_time_ + audio_stall_timeout_);

            return base_media_time_us_ + std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        }

        // 4. 如果没有音频、音频位置尚未收到或者音频已经结束了：按照单调时钟匀速推进
        const auto elapsed = SteadyClock::now() - base_system_time_;
        return base_media_time_us_ + std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    }

    /**
     * @brief 判断当前位置是否以音频进度为准
     * @details
     * 需要同时满足：媒体包含音频、已经收到音频、音频尚未结束，且距音频位置上次前进不超过卡顿判定时长
     */
    bool AudioMasterLocked() const {
        return has_audio_track_ && has_audio_position_ && !audio_finished_ &&
            (SteadyClock::now() - last_audio_progress_time_ <= audio_stall_timeout_);
    }

    /**
     * @brief 内部无锁递增状态流水号，并广播唤醒等待线程
     */
    void ChangedLocked() {
        ++playback_state_.state_version_;
        // 同步无锁副本：AudioSinkNode 的 HasPendingControl 每轮都要读版本号，
        // 若这里不更新，GetStateVersion() 会永远返回 0，HasPendingControl 恒为真，
        // 音频工作线程会一直停留在控制处理分支，永远到不了 Tick()
        state_version_atomic_.store(playback_state_.state_version_, std::memory_order_release);
        state_changed_cv_.notify_all();
    }

private:
    mutable std::mutex state_mutex_;  ///< 保护时间轴变量的锁
    std::condition_variable state_changed_cv_;  ///< 让等待中的节点在播放控制更新后重新检查
    Snapshot playback_state_;  ///< 当前播放控制状态

    const std::uint8_t media_tracks_;  ///< 实际存在的分支：0 无，1 视频，2 音频，3 两者都有
    const bool has_audio_track_;  ///< 媒体是否包含音频分支
    const std::chrono::milliseconds audio_stall_timeout_;  ///< 音频多久不前进后改用系统计

    unsigned seek_ready_tracks_{0};  ///< 本次跳转已准备好数据的分支：0 无，1 视频，2 音频，3 都有
    unsigned finished_tracks_{0};  ///< 当前播放版本已播放结束的分支：0 无，1 视频，2 音频，3 都有
    unsigned seek_end_tracks_{0};  ///< 本次跳转直接到达结尾的分支：0 无，1 视频，2 音频，3 都有

    bool seek_reported_{false};  ///< 本次跳转的结果是否已返回，避免重复返回
    bool has_audio_position_{false};  ///< 当前的播放版本是否收到过音频播放位置
    bool audio_finished_{false};  ///< 音频输出节点是否已报告播放结束

    // 按系统计时推进时，例如基准媒体位置为第 40 秒，再经过 2 秒，播放位置就是第 42 秒
    std::int64_t base_media_time_us_{0};  ///< 媒体时间基准点（微秒）
    std::int64_t aligned_seek_time_us_{0};  ///< 跳转后实际到达的位置（微秒）

    SteadyClock::time_point base_system_time_{SteadyClock::now()};  ///< 记录基准媒体位置的时的系统计时时刻
    SteadyClock::time_point last_audio_progress_time_{SteadyClock::now()};  ///< 音频进度最近一次向前推进的系统物理时刻

    std::int64_t last_audio_position_us_{0};  ///< 上次确认的音频实际发声进度位置

    std::atomic<std::uint64_t> state_version_atomic_{0};
};

}  // namespace mediacore
