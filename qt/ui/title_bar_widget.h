/**
 * @file title_bar_widget.h
 * @brief 自定义无边框窗口标题栏
 */
#pragma once

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

class TitleBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit TitleBarWidget(QWidget* parent = nullptr);

    /**
     * @brief 设置窗口标题
     * @param title 标题文本
     */
    void SetWindowTitle(const QString& title);

signals:
    /**
     * @brief 最小化按钮点击信号
     */
    void sigMinimizeClicked();

    /**
     * @brief 最大化/还原按钮点击信号
     */
    void sigMaximizeClicked();

    /**
     * @brief 关闭按钮点击信号
     */
    void sigCloseClicked();

protected:
    /**
     * @brief 鼠标双击事件（切换全屏）
     */
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    /**
     * @brief 初始化UI组件
     */
    void SetupUI();

    /**
     * @brief 建立信号槽连接
     */
    void SetupConnections();

    QLabel* m_titleLabel_;
    QPushButton* m_minimizeBtn_;
    QPushButton* m_maximizeBtn_;
    QPushButton* m_closeBtn_;
};