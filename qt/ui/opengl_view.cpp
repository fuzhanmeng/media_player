#include "opengl_view.h"

namespace av {
OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {}

OpenGLView::~OpenGLView()  = default;

void* OpenGLView::GetVideoDisplayView() {
    return this;
}

void OpenGLView::initializeGL() {
    initializeOpenGLFunctions();
    // 把画板底色刷成纯黑色 (R, G, B, Alpha)
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

}

void OpenGLView::resizeGL(int width, int hign) {
    // 当窗口大小改变时，告诉显卡我们的绘制视口也要跟着变
    glViewport(0, 0, width, hign);
}

void OpenGLView::paintGL() {
    // 清空颜色缓冲区，涂上我们在 initializeGL 里设置的黑色底色
    glClear(GL_COLOR_BUFFER_BIT);
}

} // namespace av