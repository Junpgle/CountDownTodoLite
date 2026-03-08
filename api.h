#pragma once
#include "common.h"

// 日志输出（定义在 api.cpp）
void LogMessage(const std::wstring& msg);

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

// ────────────────────────────────────────────────
// 🍅 番茄钟 API
// ────────────────────────────────────────────────

// 持久化番茄钟 Session（保存到 INI 以便重启恢复）
void SavePomodoroSession();
void LoadPomodoroSession();

// 标签同步（Delta Sync，批量上传本地 isDirty 标签）
void ApiFetchPomodoroTags();
void ApiSyncPomodoroTags();

// 记录上传（单条专注记录上传到后端）
bool ApiUploadPomodoroRecord(const PomodoroRecord &rec);

// 🚀 本地记录缓存（pomodoro_cache.json）
// SavePomodoroLocalCache：每次完成 / 中断专注后立即调用
// LoadPomodoroLocalCache：程序启动 / 登录后调用，恢复历史并上传未发送的记录
void SavePomodoroLocalCache();
void LoadPomodoroLocalCache();

// 将本地 isDirty 的记录批量上传云端（后台线程调用）
void UploadPendingPomodoroRecords();

// 拉取历史记录（按时间范围）
void ApiFetchPomodoroHistory(long long fromMs, long long toMs);

