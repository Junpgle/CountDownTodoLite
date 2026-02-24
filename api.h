#pragma once
#include "common.h"

std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body);

void ApiToggleTodo(int id, bool currentStatus);
void ApiAddTodo(const std::wstring &content);
void ApiDeleteTodo(int id);
void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr);
void ApiDeleteCountdown(int id);

// 同步屏幕使用时间并返回聚合后的今日各应用总时间
std::map<std::wstring, int> ApiSyncScreenTime(const std::map<std::wstring, int>& localData, const std::wstring& dateStr, const std::wstring& deviceName);

void SyncData();
bool AttemptAutoLogin();