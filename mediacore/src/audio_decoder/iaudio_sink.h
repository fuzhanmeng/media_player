/**
 * @file iaudio_sink
 * @brief 音频硬件输出的抽象接口：用于将来自音频解码器的 PcmFrame 帧给到 ALSA
 * @details
 * AudioSinkNode 通过此接口，可将接收到的 PCM 帧交给音频输出设备 Linux 的 ALSA 或其他设备
 * 音频格式规范：
 * - 采样率：48000 Hz
 * - 采样深度与格式：16 位有符号整型（S16LE / 主机字节序）
 * - 声道数与布局：2 声道（立体声），交错排列（Interleaved：L0, R0, L1, R1...）
 * - 数据单位：以“采样帧（Sample Frame）”为基本计量单位（1 采样帧 = 2 声道各 1 采样点 = 4 字节）
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace mediacore {

/**
 * @class IAudioSink
 * @brief 音频输出到设备的接口，规定了打开、写入、查询延迟和播放控制等操作
 * @details
 * 派生类负责持有和释放自己的设备资源，AudioSinkNode 通过 std::unique_ptr 独占该实现
 */
class IAudioSink {
public:
    /**
     * @brief 虚析构函数，可执行派生类的析构函数
     */
    virtual ~IAudioSink() = default;

    IAudioSink() = default;

    /**
     * @brief 一个设备对象对应一套播放状态，不可以复制或者移动对象本身，避免设备资源归属不清
     */
    IAudioSink(const IAudioSink&) = delete;
    IAudioSink& operator=(const IAudioSink&) = delete;

    IAudioSink(IAudioSink&&) = delete;
    IAudioSink& operator=(IAudioSink&&) = delete;

    /**
     * @brief 打开音频播放设备并设置播放格式，配置约定的硬件 PCM 参数
     * @throw std::runtime_error 设备打开失败或硬件不支持指定的标准 PCM 参数
     */
    virtual void Open() = 0;

    /**
     * @brief 向底层音频硬件环形缓冲区写入 PCM 采样数据（非阻塞）
     * @param data 待写入的数据首地址，frames > 0 时调用方必须提供足够的有效数据
     * @param frames 请求写入的采样帧数量，每帧为 2 个 16 位的数值，共 4 字节 frames * 4
     * @return 实际接收的帧数，可以小于请求值；如果暂停或者暂时无法写入可以返回 0
     * @note
     * - 实现类必须完成数据拷贝，调用返回后不得持有或者访问 data
     * - 返回成功写入不代表声音已被扬声器发出，仅表示已被驱动环形缓冲接纳
     */
    virtual std::size_t Write(const std::int16_t* data, std::size_t frames) = 0;

    /**
     * @brief 获取环形缓冲区中还有多少采样帧未被播放
     * @return 返回尚未播放的采样帧数量
     * @note 播放器根据这个数值估算声音实际播放到了什么位置，用于音画同步
     */
    virtual std::int64_t DelayFrames() = 0;

    /**
     * @brief 查询设备当前还能立即接收多少采样帧（真实空闲空间）
     * @return 设备环形缓冲区剩余可写帧数；默认返回最大值表示实现不限制
     * @note 流控必须用它，不能用 DelayFrames()：
     *       PulseAudio / PipeWire 的 ALSA 插件会把服务器端缓冲计入 snd_pcm_delay，
     *       delay 可能大于实际写入量（例如缓冲 9600 帧却报告 10333 帧待播）。
     *       此时把 delay 当作"设备占用"会让可用量恒为 0，上层停止写入，
     *       设备随即欠载，整条音频链（以及被它拖住的解封装/视频链）永久停摆。
     */
    virtual std::int64_t WritableFrames() { return std::numeric_limits<std::int64_t>::max(); }

    /**
     * @brief 轮询排空底层缓冲区中剩余的音频数据
     * @return 硬件缓冲区全部播完返回 true；仍有残存音频正在回放返回 false
     * @note 非阻塞操作，流结束时由调用方轮询调用，确保尾音完整播放而且不会阻塞主线程
     */
    virtual bool Drain() = 0;

    /**
     * @brief 强制丢弃硬件环形缓冲及内部暂存的所有未播放数据
     * @note 用于 Seek 跳转或异常恢复流程，清空旧音频残留并重置硬件回放位置
     */
    virtual void Flush() = 0;

    /**
     * @brief 切换设备的播放与暂停状态
     * @param  paused true 请求暂停回放；false 恢复播放
     * @note  ALSA 实现优先使用设备暂停；暂停时必须保留已写入缓冲但未发声的数据，恢复时从暂停处无缝续播
     */
    virtual void Pause(bool pause) = 0;

    /**
     * @brief 返回设备当前内部状态的简短描述，仅用于诊断日志
     * @return 人类可读的状态描述；默认返回空串表示实现不提供
     * @note 非阻塞、允许在任意节点线程调用；实现不得抛异常
     */
    virtual std::string DescribeState() const { return {}; }
};

/**
 * @brief 创建基于 Linux ALSA 的音频输出实例
 * @param device 设备名称：""default" 默认输出或 "plughw:0.0" 带格式适配的声卡设备
 * @param buffer_ms 请求的缓冲时长，单位为毫秒，有效范围为 [5,2000]
 * @return 拥有设备对象的 std::unique_ptr，此时只创建对象，实际打开设备需要调用 Open
 */
std::unique_ptr<IAudioSink> CreateAlsaAudioSink(std::string device, int buffer_ms);
}  // namespace mediacore