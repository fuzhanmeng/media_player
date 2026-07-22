from email import generator
from email.policy import default

from conan import ConanFile
from conan.tools.cmake import cmake_layout

class AiMediaPlayer(ConanFile):
    
    # settings：Conan 据此匹配"预编译二进制"。os / arch / compiler / build_type
    settings = "os", "compiler", "build_type", "arch"

    # generators：install 后自动生成的 CMake 集成文件
    generators = "CMakeDeps", "CMakeToolchain"

    # Qt 体量巨大，必须裁剪：不裁会编译数小时并拖入大量无关依赖
    default_options = {
        "qt/*:shared": True,          # 动态链接 Qt（静态链接有 LGPL 授权约束且体积爆炸）
        "qt/*:qtwebengine": False,    # 关闭 WebEngine（Chromium 内核，数 GB，不用）
        "qt/*:qtwebchannel": False,
        "qt/*:qtwebsockets": False,
        "qt/*:qtmultimedia": True,    # 保留 multimedia（Qt 侧多媒体能力，UI 层备用）
    }

    def requirements(self):
        # ============ 基础音视频与图形依赖 ============
        self.requires("ffmpeg/6.1.1")          # 解封装 / 编解码 / 重采样 / 缩放 核心
        self.requires("spdlog/1.15.3")         # 高性能日志
        self.requires("stb/cci.20240531")      # 轻量图像读写（截图 / 缩略图备用）
        self.requires("glm/cci.20230113")      # GL 数学库（矩阵 / 向量 → 渲染、色彩管线）

        # ============ GUI 框架 ============
        self.requires("qt/6.7.3")              # 维持：legacy 验证可解 xkbcommon 冲突

    def layout(self):
        # 标准 out-of-source 布局：生成物进 build/，源码树保持干净
        # 产出形如 build/RelWithDebInfo/generators/ 的结构
        cmake_layout(self)
