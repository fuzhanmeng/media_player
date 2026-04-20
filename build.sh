#!/bin/bash

# 默认构建类型,如果没有传参,就默认使用 RelWithDebInfo
BUILD_TYPE=${1:-RelWithDebInfo}

# 将传入的构建类型转为全小写,以匹配 Conan 生成的 CMake Preset 名字 (例如: conan-release)
PRESET_NAME="conan-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"

echo "开始构建项目: ai_media_player"
echo "构建类型: $BUILD_TYPE"
echo "使用的 CMake Preset: $PRESET_NAME"

# 1. 运行 Conan 检查并准备依赖包
echo -e "\n---> [1/3] 运行 Conan 获取依赖..."
conan install . --build=missing -s build_type=$BUILD_TYPE
if [ $? -ne 0 ]; then
    echo "Error, Conan 安装依赖失败,退出构建context_"
    exit 1
fi

# 2. 运行 CMake 配置项目
echo -e "\n---> [2/3] 运行 CMake 配置..."
cmake --preset $PRESET_NAME
if [ $? -ne 0 ]; then
    echo "Error, CMake 配置失败,退出构建context_"
    exit 1
fi

# 3. 运行 CMake 开始编译代码
echo -e "\n---> [3/3] 开始编译代码..."
cmake --build --preset $PRESET_NAME
if [ $? -ne 0 ]; then
    echo "Error, 编译失败,请检查 C++ 代码是否有语法错误context_"
    exit 1
fi

echo -e "编译成功"
echo "你可以运行: ./build/$BUILD_TYPE/ai_media_player"