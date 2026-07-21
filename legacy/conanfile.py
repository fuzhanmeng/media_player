from conan import ConanFile

class AiMediaPlayer(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    default_options = {
        "qt/*:shared": True,
        "qt/*:qtwebengine": False,
        "qt/*:qtwebchannel": False,
        "qt/*:qtwebsockets": False,
        "qt/*:qtmultimedia": True
    }

    def requirements(self):
        # 1. 基础音视频与图形库
        self.requires("ffmpeg/4.4.4")
        self.requires("stb/cci.20240531")
        self.requires("glm/cci.20230113")
        self.requires("spdlog/1.12.0")
        
        # 2. 升级到Qt 6.7.3 -- 解决 SDL2 和 QT （xkbcommon）依赖冲突
        self.requires("qt/6.7.3")

    def layout(self):
        from conan.tools.cmake import cmake_layout
        cmake_layout(self) # type: ignore