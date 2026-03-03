#pragma once
#include "common.h"

void RenderWidget();
void ResizeWidget();
// 修复：支持 3 个输出参数 (内容, 创建日期, 截止日期)
bool ShowInputDialog(HWND parent, int type, std::wstring &out1, std::wstring &out2, std::wstring &out3);
bool ShowLogin(bool isManualLogout = false);

LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);