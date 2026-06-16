/**
 * @file av_core_def.h
 * @brief FrameFlags
 */
#pragma once

namespace av {
enum AVFrameFlag {
    kNone = 0,
    kFlush = 1 << 0, // 遇到这个标志，表示发生了 Seek 跳转，下游需要清空历史缓存
    kEOS = 1 << 1, // 数据流结束
};

// 位或：组合多个标志，如 kFlush | kEOS
inline AVFrameFlag operator|(AVFrameFlag a, AVFrameFlag b) {
    return static_cast<AVFrameFlag>(static_cast<int>(a) | static_cast<int>(b));
}

// 位与：检查是否包含某个标志
inline AVFrameFlag operator&(AVFrameFlag a, AVFrameFlag b) {
    return static_cast<AVFrameFlag>(static_cast<int>(a) & static_cast<int>(b));
}

// 位或赋值：向已有标志中添加新标志
inline AVFrameFlag& operator|=(AVFrameFlag& a, AVFrameFlag b) {
    a = a | b;
    return a;
}
} // namespace av