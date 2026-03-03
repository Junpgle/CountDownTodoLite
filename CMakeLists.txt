cmake_minimum_required(VERSION 3.10)
project(MathQuizLite)

set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(UNICODE _UNICODE)

add_executable(MathQuizLite
        main.cpp
        globals.cpp
        utils.cpp
        api.cpp
        ui.cpp
        tai_reader.cpp
        sqlite3.c
        stats_window.h
        stats_window.cpp
        completed_todos_window.cpp
)

# 强制指定为 Windows GUI 子系统，隐藏控制台窗口
target_link_options(MathQuizLite PRIVATE -mwindows -static -static-libgcc -static-libstdc++)

target_link_libraries(MathQuizLite
        winhttp
        crypt32
        gdiplus
        shlwapi
        user32
        kernel32
        gdi32
        winspool
        shell32
        ole32
        oleaut32
        uuid
        comdlg32
        wininet
        advapi32
)