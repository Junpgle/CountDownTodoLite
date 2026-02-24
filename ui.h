#pragma once
#include "common.h"

void RenderWidget();
void ResizeWidget();
bool ShowInputDialog(HWND parent, int type, std::wstring &out1, std::wstring &out2);
bool ShowLogin();

LRESULT CALLBACK WidgetWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);