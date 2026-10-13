# 测试目录总入口。
# 每个子目录对应一个明确的模块或里程碑，具体编译选项和 CTest 用例在子目录中定义。

add_subdirectory(dag)

# The DAG tests build without mediacore, Qt, or FFmpeg, but still use the
# Conan-managed spdlog logger. The remaining tests link mediacore and therefore
# need the complete Conan dependency graph.
if(MEDIA_PLAYER_BUILD_MEDIACORE)
    add_subdirectory(config)
    add_subdirectory(core)
    add_subdirectory(demux)
endif()
