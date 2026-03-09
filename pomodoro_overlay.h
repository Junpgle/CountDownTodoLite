#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "common.h"   // RemoteFocusState

// ============================================================
// pomodoro_overlay.h
// 番茄钟悬浮提示窗口
// 置顶、可拖动、透明度可调的小矩形悬浮窗
// 本地专注和跨端同步均通过此窗口显示倒计时
// ============================================================

// 显示/隐藏悬浮窗
// 程序启动时调用（主线程）：预创建隐藏窗口，确保 WS 线程可以 PostMessage
void InitPomodoroOverlay();
void ShowPomodoroOverlay();
void HidePomodoroOverlay();

// 更新显示数据（每秒由计时器或 WS 推送调用）
struct OverlayInfo {
    bool  active       = false;
    int   remainSecs   = 0;     // 剩余秒数
    int   currentLoop  = 0;     // 当前轮次（1-based 显示）
    int   totalLoops   = 4;     // 总轮次
    bool  isRest       = false; // true=休息中
    bool  isRemote     = false; // true=跨端同步的专注
    std::wstring todoContent;   // 绑定的待办内容
    std::vector<std::wstring> tagNames; // 标签名列表
};

void UpdatePomodoroOverlay(const OverlayInfo& info);

// WS 接收线程调用（线程安全，内部用 PostMessage）
void NotifyOverlayRemoteFocus(const RemoteFocusState& rf);
void NotifyOverlayRemoteStop();

// 获取悬浮窗透明度（0-255），由设置界面读写
extern BYTE g_OverlayAlpha;
// 悬浮窗位置（持久化）
extern int  g_OverlayX;
extern int  g_OverlayY;

// 保存/加载悬浮窗位置和透明度
void SaveOverlaySettings();
void LoadOverlaySettings();

