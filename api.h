#pragma once
#include "common.h"

// 发送网络请求
std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body);

// 登录接口
std::string ApiLogin(const std::wstring &email, const std::wstring &password);
// 自动登录尝试
bool AttemptAutoLogin();

// 🚀 本地数据缓存持久化（Todos + Countdowns → data_cache.json）
// SaveLocalData：每次 SyncData 合并完成后自动调用
// LoadLocalData：程序启动登录成功后立即调用，让界面无需等待网络即可显示数据
void SaveLocalData();
void LoadLocalData();

// 待办操作
void ApiAddTodo(const std::wstring &content, const std::wstring &createdDate, const std::wstring &dueDate, bool isDone, const std::wstring &remark = L"");
void ApiUpdateTodo(const std::wstring &uuid, const std::wstring &content, const std::wstring &createdDate, const std::wstring &dueDate, bool isDone, const std::wstring &remark = L"");
void ApiToggleTodo(int id, bool done);
void ApiToggleTodoByUuid(const std::wstring &uuid, bool done);
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

// 课表同步
void ApiFetchCourses();
void SaveLocalCourses();
void LoadLocalCourses();

