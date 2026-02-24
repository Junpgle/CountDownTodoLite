cmake_minimum_required(VERSION 3.10)
project(MathQuizLite)

set(CMAKE_CXX_STANDARD 17)

# 确保支持 Unicode
add_compile_definitions(UNICODE _UNICODE)

# 把所有 .cpp 和 .c 源文件都加进来
add_executable(MathQuizLite
        main.cpp
        globals.cpp
        utils.cpp
        api.cpp
        ui.cpp
        tai_reader.cpp
        sqlite3.c
)

# 链接程序所需的所有系统库
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
        advapi32
)