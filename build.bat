@echo off
REM 这是一个简单的使用 MSVC 的编译脚本
REM 请在 "x64 Native Tools Command Prompt for VS" 中运行此脚本

echo 正在编译 MathQuizLite Tai数据版...

cl.exe /O2 /EHsc /W3 /DUNICODE /D_UNICODE ^
    main.cpp globals.cpp utils.cpp api.cpp ui.cpp tai_reader.cpp sqlite3.c ^
    /link User32.lib Gdi32.lib Winhttp.lib Crypt32.lib Gdiplus.lib Shlwapi.lib Advapi32.lib Ole32.lib Kernel32.lib Shell32.lib ^
    /out:MathQuizLite.exe

if %errorlevel% equ 0 (
    echo 编译成功！生成 MathQuizLite.exe
) else (
    echo 编译失败，请检查错误信息。
)
pause