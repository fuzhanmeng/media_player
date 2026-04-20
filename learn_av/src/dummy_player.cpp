#include <atomic>
#include <iostream>
#include <thread>

#include "iplayer.h"

namespace av {

/**
 * @brief 假播放器实现类
 * 仅仅为了占位，实现iplayer纯虚函数，要不然编译不过
 */
class DummyPlayer : public IPlayer {
public:
    explicit DummyPlayer(void* context) : context_(context), playing_(false) {
        std::cout << "DummyPlayer 已创建接收到的 GL 上下文句柄: " << context_ << std::endl;
    }

    ~DummyPlayer() override { std::cout << "DummyPlayer 已销毁" << std::endl; }

    // --- 界面绑定接口 ---

    void AttachDisplayView(void* view_handle) override {
        std::cout << "画面已挂载到视图句柄: " << view_handle << std::endl;
    }

    void DetachDisplayView(void* view_handle) override {
        std::cout << "画面已从句柄 " << view_handle << " 解绑" << std::endl;
    }

    // --- 播放控制接口 ---

    void Play() override {
        playing_ = true;
        std::cout << "执行指令: 开始播放" << std::endl;
    }

    void Pause() override {
        playing_ = false;
        std::cout << "执行指令: 暂停播放" << std::endl;
    }

    bool IsPlaying() const override { return playing_; }

    void SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) override {
        listener_ = listener;
        std::cout << "监听器已注册" << std::endl;
    }

private:
    void* context_; // 存储 OpenGL 上下文句柄
    std::atomic<bool> playing_; // 播放状态原子变量
    std::shared_ptr<IPlaybackListener> listener_; // 进度监听器
};

// 实现 iplayer.h 中定义的工厂方法，只有在这里，我们才知道要实例化哪个具体的类
std::shared_ptr<av::IPlayer> IPlayer::Create(void* shared_gl_context) {
    // 实例化 DummyPlayer 并作为 IPlayer 接口返回
    // 这样 UI 层只拿到了接口指针，根本不知道 DummyPlayer 的存在
    return std::make_shared<DummyPlayer>(shared_gl_context);
}

} // namespace av