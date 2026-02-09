cmake_minimum_required(VERSION 3.10)
project(MathQuizLite)

set(CMAKE_CXX_STANDARD 17)

# 包含 json.hpp 的路径（如果它在当前目录下，"." 就足够了）
include_directories(.)

# 添加可执行文件
add_executable(MathQuizLite main.cpp)

# 针对 Windows 的链接库配置
if(WIN32)
    # 链接必要的 Windows 系统库
    target_link_libraries(MathQuizLite
            winhttp
            crypt32
            gdiplus
            shlwapi
            user32
    )

    # 如果是 MinGW 编译器，添加静态链接参数，
    # 这样生成的 exe 可以发给别人运行，不需要缺 dll
    if(MINGW)
        target_link_options(MathQuizLite PRIVATE
                -static
                -mwindows  # 这一行会让程序运行时不显示黑色控制台窗口
        )
    endif()
endif()