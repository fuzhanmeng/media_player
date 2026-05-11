/**
 * @file idemuxer.h
 * @brief 解复用引擎控制接口
 */

#pragma once

#include <memory>
#include <string>

namespace av {

// 前置声明监听器接口
class IDemuxerListener;

class IDemuxer {
public:
    virtual ~IDemuxer() = default;

    /**
     * @brief 注册监听器 (绑定下游接收方)
     * 必须在 Open 之前调用，否则漏掉流初始化信息
     */
    virtual void SetListener(IDemuxerListener* listener) = 0;

    /**
     * @brief 打开媒体文件并解析流信息
     * @param url 文件本地路径或网络流地址
     * @return 成功返回 true，失败返回 false
     */
    virtual bool Open(const std::string& url) = 0;

    /**
     * @brief 启动解复用后台线程
     */
    virtual void Start() = 0;

    /**
     * @brief 暂停解复用线程 (可选操作，用于配合整体播放暂停)
     */
    virtual void Pause() = 0;

    /**
     * @brief 停止解复用线程并释放资源
     */
    virtual void Stop() = 0;

    /**
     * @brief 进度跳转
     * @param progress 目标进度比例 (0.0f ~ 1.0f)
     */
    virtual void SeekTo(float progress) = 0;

    /**
     * @brief 获取媒体总时长
     * @return 总时长（单位：秒 或 毫秒，由具体实现决定，建议统一为秒的浮点数）
     */
    virtual float GetDuration() = 0;

    /**
     * @brief 获取解复用器实例
     */
    static std::unique_ptr<IDemuxer> Create();
};

} // namespace av