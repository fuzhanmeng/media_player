/**
 * @brief 视频渲染容器组件
 * 负责管理底层的 OpenGLView，并在自身生命周期内，
 * 自动完成与 IPlayer 核心引擎的画面绑定与解绑。
 */

#include <QWidget> // 所有用户界面对象的基类
#include <memory>

#pragma once

namespace av {
class IPlayer;
class OpenGLView;
}


class PlayerWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlayerWidget(QWidget* parent, std::shared_ptr<av::IPlayer> player);
    ~PlayerWidget();

private:
    std::shared_ptr<av::IPlayer> m_player;
    av::OpenGLView* m_opengl_view{nullptr};
};