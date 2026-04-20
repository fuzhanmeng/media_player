#!/bin/bash

echo "清理构建目录和缓存..."

# 删除 CMake 构建目录
if [ -d "build" ]; then
    rm -rf build
    echo " -> 已删除 build/ 目录"
fi

# 删除 Conan 生成的 Preset 文件
if [ -f "CMakeUserPresets.json" ]; then
    rm -f CMakeUserPresets.json
    echo " -> 已删除 CMakeUserPresets.json"
fi

echo "清理完成"