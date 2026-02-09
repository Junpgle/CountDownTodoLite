cmake_minimum_required(VERSION 3.16)

project(MathQuizLite VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_UNICODE ON) # 使用 Unicode (宽字符) API

# 源文件
set(PROJECT_SOURCES
        main.cpp
)

add_executable(MathQuizLite ${PROJECT_SOURCES})

# 链接 Windows 原生库
# winhttp: HTTP请求
# crypt32: 密码哈希 (SHA256)
# gdiplus: 绘制半透明窗口和文字
# shlwapi: 路径处理
target_link_libraries(MathQuizLite PRIVATE winhttp crypt32 gdiplus shlwapi user32 kernel32 gdi32)

# Windows 子系统设置 (发布时不显示黑框控制台)
if(WIN32)
    set_target_properties(MathQuizLite PROPERTIES WIN32_EXECUTABLE ON)
endif()