#pragma once
#include "common.h"

// 发送网络请求
std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body);

// 登录接口
std::string ApiLogin(const std::wstring &email, const std::wstring &password);
// 自动登录尝试
bool AttemptAutoLogin();

// 待办操作 - 修复：添加更多参数以匹配最新 UI
void ApiAddTodo(const std::wstring &content, const std::wstring &createdDate, const std::wstring &dueDate, bool isDone);
void ApiToggleTodo(int id, bool done);
void ApiDeleteTodo(int id);

// 倒计时操作
void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr);
void ApiDeleteCountdown(int id);

// 同步屏幕时间
std::map<std::wstring, int> ApiSyncScreenTime(const std::map<std::wstring, int>& localData, const std::wstring& dateStr, const std::wstring& deviceName);

// 同步账号状态
bool ApiFetchUserStatus();

// 全局同步任务
void SyncData();