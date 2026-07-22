# CMakeLists.txt 

# cmake 版本号定义
cmake_minimum_required(VERSION 3.23)
project(ai_media_player VERSION 1.0.0 LANGUAGES CXX)

# 全局 C++ 标准策略
# STANDARD 17       → 用 C++17
# STANDARD_REQUIRED → 编译器不支持 17 就报错，不许偷偷降级
# EXTENSIONS OFF    → 关 GNU 扩展(-std=gnu++17)，强制标准 -std=c++17，保跨平台
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json,给 clangd/VSCode 做跳转与静态分析
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Qt 代码生成流水线
# AUTOMOC：自动对含 Q_OBJECT 的头跑 moc 预处理器（signal/slot 靠它，漏了会链接报错）
# AUTORCC：自动处理 .qrc 资源文件
# AUTOUIC：自动处理 .ui 界面文件
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 把应用层子目录纳入构建
add_subdirectory(app)