from conan import ConanFile
from conan.tools.cmake import cmake_layout

required_conan_version = ">=2.0"


class AiMediaPlayer(ConanFile):
    # settings：Conan 据此匹配"预编译二进制"。os / arch / compiler / build_type
    settings = "os", "compiler", "build_type", "arch"

    # generators：install 后自动生成的 CMake 集成文件
    generators = "CMakeDeps", "CMakeToolchain", "VirtualRunEnv"

    # FFmpeg is deliberately kept separate from the system ffmpeg/ffprobe CLI.
    # The project links only Conan-provided libav libraries. Static linking also
    # prevents the runtime loader from resolving an apt libav library by SONAME.
    # Review the LGPL relink requirements before distributing a static binary.
    # FFmpeg 8.0 removed the postproc option; do not add ffmpeg/*:postproc here.
    # The external encoders below are disabled because the current sprint does
    # not implement recording, and libx264/libx265/libfdk_aac would add
    # GPL/nonfree licensing requirements to the dependency graph. Revisit this
    # profile before implementing the recording branch from the full plan.
    default_options = {
        "ffmpeg/*:shared": False,
        "ffmpeg/*:avcodec": True,
        "ffmpeg/*:avformat": True,
        "ffmpeg/*:swresample": True,
        "ffmpeg/*:swscale": True,
        "ffmpeg/*:avdevice": False,
        "ffmpeg/*:avfilter": False,
        "ffmpeg/*:with_asm": True,
        "ffmpeg/*:with_zlib": True,
        "ffmpeg/*:with_bzip2": True,
        "ffmpeg/*:with_lzma": True,
        "ffmpeg/*:with_libiconv": True,
        "ffmpeg/*:with_programs": False,
        "ffmpeg/*:with_ssl": "openssl",
        "ffmpeg/*:with_vaapi": True,
        "ffmpeg/*:with_vdpau": False,
        "ffmpeg/*:with_vulkan": False,
        # Required for VA-API DRM PRIME export (DMA-BUF/EGLImage). Conan owns
        # this build dependency; the GPU driver and DRM device remain on host.
        "ffmpeg/*:with_libdrm": True,
        "ffmpeg/*:with_libalsa": False,
        "ffmpeg/*:with_pulse": False,
        "ffmpeg/*:with_xcb": False,
        "ffmpeg/*:with_xlib": False,
        "ffmpeg/*:with_freetype": False,
        "ffmpeg/*:with_openjpeg": False,
        "ffmpeg/*:with_openh264": False,
        "ffmpeg/*:with_opus": False,
        "ffmpeg/*:with_vorbis": False,
        "ffmpeg/*:with_libx264": False,
        "ffmpeg/*:with_libx265": False,
        "ffmpeg/*:with_libvpx": False,
        "ffmpeg/*:with_libmp3lame": False,
        "ffmpeg/*:with_libfdk_aac": False,
        "ffmpeg/*:with_libwebp": False,
        "ffmpeg/*:with_libsvtav1": False,
        "ffmpeg/*:with_libaom": False,
        "ffmpeg/*:with_libdav1d": False,
        "ffmpeg/*:with_soxr": False,
        "ffmpeg/*:with_sdl": False,
        "ffmpeg/*:with_zeromq": False,
        "ffmpeg/*:with_libxml2": False,
        "ffmpeg/*:with_fontconfig": False,
        "ffmpeg/*:with_fribidi": False,
        "ffmpeg/*:with_harfbuzz": False,
        "ffmpeg/*:with_libjxl": False,
        "ffmpeg/*:with_whisper": False,
        "ffmpeg/*:with_openapv": False,

        # Qt 体量巨大，必须裁剪：不裁会编译数小时并拖入大量无关依赖
        "qt/*:shared": True,          # 动态链接 Qt（静态链接有 LGPL 授权约束且体积爆炸）
        "qt/*:qtwebengine": False,    # 关闭 WebEngine（Chromium 内核，数 GB，不用）
        "qt/*:qtwebchannel": False,
        "qt/*:qtwebsockets": False,
        "qt/*:qtmultimedia": False,   # 音频计划直连 ALSA，不引入 Qt Multimedia 媒体栈
        # 项目不使用 QtSql；关闭数据库驱动，避免引入 PostgreSQL、ODBC 和 SQLite。
        "qt/*:with_sqlite3": False,
        "qt/*:with_mysql": False,
        "qt/*:with_pq": False,
        "qt/*:with_odbc": False,
    }

    def requirements(self):
        # ============ 基础音视频与图形依赖 ============
        self.requires("ffmpeg/8.0.1")          # 解封装 / 编解码 / 重采样 / 缩放 / VA-API 核心
        self.requires("spdlog/1.15.3")         # 高性能日志
        self.requires("glm/cci.20230113")      # GL 数学库（矩阵 / 向量 → 渲染、色彩管线）

        # ============ GUI 框架 ============
        self.requires("qt/6.7.3")              # 维持：legacy 验证可解 xkbcommon 冲突

    def layout(self):
        # 标准 out-of-source 布局：生成物进 build/，源码树保持干净
        # 产出形如 build/RelWithDebInfo/generators/ 的结构
        cmake_layout(self)
