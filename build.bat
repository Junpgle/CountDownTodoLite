@echo off
chcp 65001
echo ==========================================
echo       Math Quiz Lite 自动编译工具
echo ==========================================
echo.

:: 检查是否安装了 g++ (MinGW)
where g++ >nul 2>nul
if %errorlevel% neq 0 (
    echo [错误] 未检测到 g++ 编译器。
    echo 请确保安装了 MinGW-w64 并将其 bin 目录添加到了系统环境变量 Path 中。
    echo.
    echo 如果你安装的是 Dev-C++ 或 Code::Blocks，请找到 g++.exe 的位置。
    pause
    exit /b
)

:: 检查源码文件是否存在
if not exist "math_quiz_lite.cpp" (
    echo [错误] 未找到 math_quiz_lite.cpp 文件。
    echo 请确保源码文件在当前目录下。
    pause
    exit /b
)

:: 检查依赖库是否存在
if not exist "json.hpp" (
    echo [错误] 未找到 json.hpp 文件。
    echo 请下载 nlohmann/json 库并将其重命名为 json.hpp 放在当前目录下。
    echo 下载地址: https://github.com/nlohmann/json/releases
    pause
    exit /b
)

echo [1/2] 正在编译中，请稍候...
echo 编译命令: g++ math_quiz_lite.cpp -o MathQuizLite.exe ...

:: 执行编译命令
:: -std=c++17: 使用 C++17 标准
:: -static: 静态链接，确保 exe 在没有 dll 的电脑上也能运行
:: -mwindows: 隐藏控制台窗口 (黑框)
:: -l...: 链接 Windows 系统库
g++ math_quiz_lite.cpp -o MathQuizLite.exe -std=c++17 -static -mwindows -lwinhttp -lcrypt32 -lgdiplus -lshlwapi -luser32 -lole32 -luuid

if %errorlevel% equ 0 (
    echo.
    echo [2/2] 编译成功！
    echo 生成文件: MathQuizLite.exe
    echo.
    echo 你现在可以直接运行 MathQuizLite.exe 了。
) else (
    echo.
    echo [失败] 编译过程中出现错误，请检查代码或环境。
)

pause