#!/bin/bash

# 每次运行必定强制清理 build 目录
echo "强制清理历史构建目录..."
rm -rf build
echo "清理完成！"

# 默认构建类型,如果没有传参,就默认使用 RelWithDebInfo
BUILD_TYPE=${1:-RelWithDebInfo}

# 将传入的构建类型转为全小写,以匹配 Conan 生成的 CMake Preset 名字 (例如: conan-release)
PRESET_NAME="conan-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"

echo "开始构建项目: media_player"
echo "构建类型: $BUILD_TYPE"
echo "使用的 CMake Preset: $PRESET_NAME"

# 1. 运行 Conan 检查并准备依赖包
echo -e "\n---> [1/3] 运行 Conan 获取依赖..."
if [ ! -f "conan.lock" ]; then
    echo "Error, conan.lock 不存在；请先生成并提交锁文件"
    exit 1
fi

# CMakeUserPresets.json 是 Conan 生成的本地文件。清除旧 include，避免曾用过
# 其他输出目录时把同名 conan preset 重复载入。
rm -f CMakeUserPresets.json

# freedesktop.org 对非浏览器下载返回 HTTP 418。这里从可信镜像预取同一份
# fontconfig 归档，并由 Conan recipe 中相同的 SHA-256 继续校验和使用。
CONAN_SOURCE_CACHE="$(pwd)/cache/conan-sources"
FONTCONFIG_SHA256="63a0658d0e06e0fa886106452b58ef04f21f58202ea02a94c39de0d3335d7c0e"
FONTCONFIG_CACHE_FILE="$CONAN_SOURCE_CACHE/s/$FONTCONFIG_SHA256"
FONTCONFIG_MIRRORS=(
    "https://deb.debian.org/debian/pool/main/f/fontconfig/fontconfig_2.15.0.orig.tar.xz"
    "https://distfiles.macports.org/fontconfig/fontconfig-2.15.0.tar.xz"
)

prepare_fontconfig_source() {
    if [ -f "$FONTCONFIG_CACHE_FILE" ] &&
       printf '%s  %s\n' "$FONTCONFIG_SHA256" "$FONTCONFIG_CACHE_FILE" |
           sha256sum --check --status; then
        return 0
    fi

    if ! command -v curl >/dev/null 2>&1; then
        echo "Error, 缺少 curl，无法下载 fontconfig 源码"
        return 1
    fi

    mkdir -p "$(dirname "$FONTCONFIG_CACHE_FILE")" || return 1

    local temp_file
    temp_file=$(mktemp "$FONTCONFIG_CACHE_FILE.tmp.XXXXXX") || return 1

    local mirror
    local downloaded=false
    echo "准备 Conan fontconfig 2.15.0 源缓存..."
    for mirror in "${FONTCONFIG_MIRRORS[@]}"; do
        if curl --fail --location --retry 2 --retry-delay 2 \
                --connect-timeout 15 --max-time 120 \
                --output "$temp_file" "$mirror"; then
            downloaded=true
            break
        fi
        echo "Warning, fontconfig 镜像下载失败: $mirror"
    done

    if [ "$downloaded" != true ]; then
        rm -f "$temp_file"
        echo "Error, 所有 fontconfig 镜像均下载失败"
        return 1
    fi

    if ! printf '%s  %s\n' "$FONTCONFIG_SHA256" "$temp_file" |
         sha256sum --check --status; then
        rm -f "$temp_file"
        echo "Error, fontconfig 源码 SHA-256 校验失败"
        return 1
    fi

    mv "$temp_file" "$FONTCONFIG_CACHE_FILE"
}

if ! prepare_fontconfig_source; then
    echo "Error, Conan 源缓存准备失败,退出构建"
    exit 1
fi

conan install . --lockfile=conan.lock --build=missing \
    -s build_type="$BUILD_TYPE" \
    -cc "core.sources:download_cache=$CONAN_SOURCE_CACHE"
if [ $? -ne 0 ]; then
    echo "Error, Conan 安装依赖失败,退出构建"
    exit 1
fi

# 2. 运行 CMake 配置项目
echo -e "\n---> [2/3] 运行 CMake 配置..."
cmake --preset $PRESET_NAME
if [ $? -ne 0 ]; then
    echo "Error, CMake 配置失败,退出构建"
    exit 1
fi

# 3. 运行 CMake 开始编译代码
# 自动获取 CPU 核心数
CORES=$(nproc)
echo -e "\n---> [3/3] 开始编译代码 使用 $CORES 线程"
cmake --build --preset $PRESET_NAME -j$CORES
if [ $? -ne 0 ]; then
    echo "Error, 编译失败,请检查 C++ 代码是否有语法错误"
    exit 1
fi

echo -e "\n编译成功！"
echo "运行前请加载 Conan 运行时环境:"
echo "source build/$BUILD_TYPE/generators/conanrun.sh"
echo "./build/$BUILD_TYPE/app/media_player"
