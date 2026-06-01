/**
 * @file igl_context.h
 * @brief 跨平台 OpenGL 上下文包装器
 */
#pragma once

namespace av {
struct IGLContext {
    void* handle_{nullptr};

    explicit IGLContext(void* handle) : handle_(handle) {}

    virtual ~IGLContext() = default;
};
} // namespace av