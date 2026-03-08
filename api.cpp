#include "api.h"
#include "utils.h"
#include "common.h"
#include "tai_reader.h"
#include <winhttp.h>
#include <ctime>
#include <debugapi.h>
#include <iostream>
#include <thread>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

using json = nlohmann::json;

/**
 * 内部辅助：统一日志输出
 */
void LogMessage(const std::wstring &msg) {
    OutputDebugStringW(L"\n[MathQuiz] ");
    OutputDebugStringW(msg.c_str());

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode;
        if (GetConsoleMode(hOut, &dwMode)) {
            std::wstring fullMsg = L"[API LOG] " + msg + L"\n";
            WriteConsoleW(hOut, fullMsg.c_str(), (DWORD) fullMsg.length(), NULL, NULL);
            return;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "[API LOG] " << ToUtf8(msg) << std::endl;
    std::cout.flush();
}

/**
 * 🚀 内部辅助：保存同步状态到本地 INI 文件
 */
void SaveSyncStatusToLocal() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    WritePrivateProfileStringW(L"Auth", L"UserTier", g_UserTier.c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"SyncCount", std::to_wstring(g_SyncCount).c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"SyncLimit", std::to_wstring(g_SyncLimit).c_str(), path);
}

/**
 * 🚀 核心新增：将内存中的课程表保存至本地 JSON
 */
void SaveLocalCourses() {
    try {
        json jArr = json::array(); {
            std::lock_guard<std::recursive_mutex> l(g_DataMutex);
            for (const auto &c: g_Courses) {
                json j;
                j["id"] = c.id;
                j["course_name"] = ToUtf8(c.courseName);
                j["room_name"] = ToUtf8(c.roomName);
                j["teacher_name"] = ToUtf8(c.teacherName);
                j["start_time"] = c.startTime;
                j["end_time"] = c.endTime;
                j["weekday"] = c.weekday;
                j["week_index"] = c.weekIndex;
                j["lesson_type"] = ToUtf8(c.lessonType);
                jArr.push_back(j);
            }
        }
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        PathRemoveFileSpecW(path);
        PathAppendW(path, L"courses_cache.json");

        HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string s = jArr.dump();
            DWORD written;
            WriteFile(hFile, s.c_str(), (DWORD) s.length(), &written, NULL);
            CloseHandle(hFile);
            LogMessage(L"已成功将云端课表缓存至本地!");
        }
    } catch (...) {
        LogMessage(L"保存本地课表缓存失败");
    }
}

/**
 * 🚀 核心新增：从本地 JSON 读取课程表
 */
void LoadLocalCourses() {
    try {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        PathRemoveFileSpecW(path);
        PathAppendW(path, L"courses_cache.json");

        HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(hFile, NULL);
            if (size > 0) {
                std::string s(size, '\0');
                DWORD read;
                ReadFile(hFile, &s[0], size, &read, NULL);

                auto jArr = json::parse(s);
                if (jArr.is_array()) {
                    std::vector<Course> temp;
                    for (auto &it: jArr) {
                        Course c;
                        c.id = it.value("id", 0);
                        c.courseName = ToWide(it.value("course_name", ""));
                        c.roomName = ToWide(it.value("room_name", ""));
                        c.teacherName = ToWide(it.value("teacher_name", ""));
                        c.startTime = it.value("start_time", 0);
                        c.endTime = it.value("end_time", 0);
                        c.weekday = it.value("weekday", 1);
                        c.weekIndex = it.value("week_index", 1);
                        c.lessonType = ToWide(it.value("lesson_type", ""));
                        temp.push_back(c);
                    }
                    std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                    g_Courses = temp;
                    LogMessage(L"成功从本地缓存加载了 " + std::to_wstring(temp.size()) + L" 节课程");
                }
            }
            CloseHandle(hFile);
        } else {
            LogMessage(L"未发现本地课表缓存文件");
        }
    } catch (...) {
        LogMessage(L"解析本地课表缓存失败");
    }
}

// ============================================================
// 🚀 本地数据缓存：Todos + Countdowns 持久化到 data_cache.json
// 目的：支持 Delta Sync 重启后恢复本地数据，无需等待网络即可显示
// ============================================================

// 内部辅助：获取 data_cache.json 的完整路径
static std::wstring GetDataCachePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"data_cache.json");
    return path;
}

void SaveLocalData() {
    try {
        json root;
        root["version"]    = 4; // v4：添加 remark 备注字段
        root["saved_at"]   = (long long)time(nullptr) * 1000LL;
        root["user_id"]    = g_UserId;

        json todosArr = json::array();
        json cdsArr   = json::array();

        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

            for (const auto &t : g_Todos) {
                json j;
                j["id"]           = t.id;
                j["uuid"]         = ToUtf8(t.uuid);
                j["content"]      = ToUtf8(t.content);
                j["is_done"]      = t.isDone;
                j["is_deleted"]   = t.isDeleted;
                j["is_dirty"]     = t.isDirty;
                j["last_updated"] = (long long)t.lastUpdated * 1000LL;
                // 日期字段：存为 UTC ms 时间戳（0 表示空）
                j["created_date_ms"] = DateStringToUtcMs(t.createdDate);
                j["due_date_ms"]     = DateStringToUtcMs(t.dueDate);
                // 循环字段
                j["recurrence"]           = t.recurrence;
                j["custom_interval_days"] = t.customIntervalDays;
                j["recurrence_end_ms"]    = DateStringToUtcMs(t.recurrenceEndDate);
                // 备注字段
                j["remark"]               = ToUtf8(t.remark);
                todosArr.push_back(j);
            }

            for (const auto &c : g_Countdowns) {
                json j;
                j["id"]           = c.id;
                j["uuid"]         = ToUtf8(c.uuid);
                j["title"]        = ToUtf8(c.title);
                j["is_dirty"]     = c.isDirty;
                j["last_updated"] = (long long)c.lastUpdated * 1000LL;
                // target_time 存为 UTC ms
                j["target_time_ms"] = DateStringToUtcMs(c.dateStr);
                cdsArr.push_back(j);
            }
        }

        root["todos"]      = todosArr;
        root["countdowns"] = cdsArr;

        std::wstring cachePath = GetDataCachePath();
        HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string s = root.dump();
            DWORD written;
            WriteFile(hFile, s.c_str(), (DWORD)s.length(), &written, NULL);
            CloseHandle(hFile);
            LogMessage(L"本地数据已缓存 (Todos:" + std::to_wstring(g_Todos.size())
                     + L", Countdowns:" + std::to_wstring(g_Countdowns.size()) + L")");
        }
    } catch (...) {
        LogMessage(L"SaveLocalData 失败");
    }
}

void LoadLocalData() {
    std::wstring cachePath = GetDataCachePath();
    HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogMessage(L"未找到本地数据缓存，首次启动或缓存已清除");
        return;
    }

    DWORD size = GetFileSize(hFile, NULL);
    if (size == 0 || size > 10 * 1024 * 1024) { // 超过 10MB 视为损坏
        CloseHandle(hFile);
        LogMessage(L"LoadLocalData：缓存文件异常，跳过");
        return;
    }

    std::string s(size, '\0');
    DWORD read;
    ReadFile(hFile, &s[0], size, &read, NULL);
    CloseHandle(hFile);

    try {
        auto root = json::parse(s);

        // 版本检查：低于 v4 的旧格式直接丢弃（字段不兼容）
        int version = root.value("version", 1);
        if (version < 4) {
            LogMessage(L"LoadLocalData：旧版缓存格式(v" + std::to_wstring(version) + L")，忽略（将在下次同步后重建）");
            // 🚀 关键修复：同时重置 last_sync_time，强制下次同步做全量拉取
            g_LastSyncTime = 0;
            SaveLastSyncTime(0);
            return;
        }

        // 用户 ID 校验：防止登录不同账号时加载到别人的数据
        int cachedUserId = root.value("user_id", 0);
        if (cachedUserId != g_UserId) {
            LogMessage(L"LoadLocalData：缓存 userId 不匹配，跳过");
            g_LastSyncTime = 0;
            SaveLastSyncTime(0);
            return;
        }

        std::vector<Todo>      tempTodos;
        std::vector<Countdown> tempCds;

        if (root.contains("todos") && root["todos"].is_array()) {
            for (const auto &j : root["todos"]) {
                Todo t;
                t.id          = j.value("id", 0);
                t.uuid        = ToWide(j.value("uuid", ""));
                t.content     = ToWide(j.value("content", ""));
                t.isDone      = j.value("is_done", false);
                t.isDeleted   = j.value("is_deleted", false);
                t.isDirty     = j.value("is_dirty", false);
                t.lastUpdated = (time_t)(j.value("last_updated", 0LL) / 1000LL);

                long long createdMs = j.value("created_date_ms", 0LL);
                long long dueMs     = j.value("due_date_ms", 0LL);
                t.createdDate = UtcMsToDateString(createdMs);
                t.dueDate     = UtcMsToDateString(dueMs);

                t.recurrence         = j.value("recurrence", 0);
                t.customIntervalDays = j.value("custom_interval_days", 0);
                long long recEndMs   = j.value("recurrence_end_ms", 0LL);
                t.recurrenceEndDate  = UtcMsToDateString(recEndMs);
                t.remark             = ToWide(j.value("remark", ""));

                tempTodos.push_back(t);
            }
        }

        if (root.contains("countdowns") && root["countdowns"].is_array()) {
            for (const auto &j : root["countdowns"]) {
                Countdown c;
                c.id          = j.value("id", 0);
                c.uuid        = ToWide(j.value("uuid", ""));
                c.title       = ToWide(j.value("title", ""));
                c.isDirty     = j.value("is_dirty", false);
                c.lastUpdated = (time_t)(j.value("last_updated", 0LL) / 1000LL);

                long long targetMs = j.value("target_time_ms", 0LL);
                c.dateStr  = UtcMsToDateOnly(targetMs);
                c.daysLeft = CalculateDaysLeft(c.dateStr);

                tempCds.push_back(c);
            }
        }

        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            g_Todos      = tempTodos;
            g_Countdowns = tempCds;
        }

        LogMessage(L"本地缓存加载完成 (Todos:" + std::to_wstring(tempTodos.size())
                 + L", Countdowns:" + std::to_wstring(tempCds.size()) + L")");

    } catch (...) {
        LogMessage(L"LoadLocalData：JSON 解析失败，缓存可能已损坏");
    }
}

std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"MathQuizLite/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "ERROR: Session failed";

    HINTERNET hConnect = WinHttpConnect(hSession, API_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return "ERROR: Connect failed"; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, ToWide(method).c_str(), path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "ERROR: Request failed";
    }

    // 连接超时 + 接收超时
    DWORD connectTimeout = 8000;
    DWORD receiveTimeout = 15000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receiveTimeout, sizeof(receiveTimeout));

    // 🚀 统一请求头：优先使用 Bearer Token，无 Token 时降级到旧 x-user-id
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!g_AuthToken.empty()) {
        headers += L"Authorization: Bearer " + g_AuthToken + L"\r\n";
    } else if (g_UserId > 0) {
        // 兼容未登录场景（如更新检查）
        headers += L"x-user-id: " + std::to_wstring(g_UserId) + L"\r\n";
    }

    if (WinHttpSendRequest(hRequest, headers.c_str(), -1,
            (LPVOID)body.c_str(), (DWORD)body.length(), (DWORD)body.length(), 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0;
            do {
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;
                char *buf = new char[dwSize + 1];
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, (LPVOID)buf, dwSize, &dwDownloaded)) {
                    buf[dwDownloaded] = '\0';
                    response += buf;
                }
                delete[] buf;
            } while (dwSize > 0);
        }
    } else {
        response = "ERROR: Network Error";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

std::string ApiLogin(const std::wstring &email, const std::wstring &password) {
    json j;
    j["email"]    = ToUtf8(email);
    j["password"] = ToUtf8(password);
    std::string res = SendRequest(L"/api/auth/login", "POST", j.dump());
    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            g_UserId   = resp["user"]["id"].get<int>();
            g_Username = ToWide(resp["user"]["username"].get<std::string>());
            if (resp["user"].contains("tier"))
                g_UserTier = ToWide(resp["user"]["tier"].get<std::string>());

            // 🚀 保存 Bearer Token
            if (resp.contains("token"))
                g_AuthToken = ToWide(resp["token"].get<std::string>());

            return "SUCCESS";
        }
        if (resp.contains("error")) return resp["error"].get<std::string>();
    } catch (...) {}
    return "FAILED";
}

bool AttemptAutoLogin() {
    if (g_SavedEmail.empty() || g_SavedPass.empty()) return false;
    return ApiLogin(g_SavedEmail, g_SavedPass) == "SUCCESS";
}

/**
 * 🚀 模块 B：本地乐观写操作（不单独发网络请求，标记 isDirty 待批量上传）
 *
 * 设计说明：
 *   新后端的所有写入均通过 /api/sync POST 批量提交。
 *   因此 Add/Toggle/Delete 只在内存中操作并设 isDirty=true，
 *   由下一次 SyncData() 调用时统一携带上传。
 *   UI 层在调用这些函数后仍需调用 SyncData() 触发实际同步。
 */

 // 内部辅助：生成本地临时 UUID（格式与后端兼容）
static std::wstring GenLocalUuid() {
    // 只初始化一次随机种子，避免同一秒内调用时重复
    static bool s_seeded = false;
    if (!s_seeded) {
        srand((unsigned int)(time(nullptr) ^ (uintptr_t)GetCurrentThreadId() ^ (uintptr_t)GetCurrentProcessId()));
        s_seeded = true;
    }
    wchar_t buf[40];
    swprintf_s(buf,
        L"pc%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
        (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF,
        (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF,
        (unsigned)rand()&0xFFFF,
        (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF, (unsigned)rand()&0xFFFF);
    return buf;
}

void ApiAddTodo(const std::wstring &content, const std::wstring &createdDate,
                const std::wstring &dueDate, bool isDone, const std::wstring &remark) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    Todo t;
    t.id          = -(int)(g_Todos.size() + 1);
    t.uuid        = GenLocalUuid();
    t.content     = content;
    t.isDone      = isDone;
    t.lastUpdated = time(nullptr);
    t.createdDate = createdDate;
    t.dueDate     = dueDate;
    t.remark      = remark;
    t.isDirty     = true;
    g_Todos.push_back(t);
    LogMessage(L"本地新增待办（待同步）: " + content);
}

void ApiToggleTodo(int id, bool done) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (t.id == id) {
            t.isDone      = done;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地切换待办状态（待同步）: " + t.content);
            return;
        }
    }
}

void ApiToggleTodoByUuid(const std::wstring &uuid, bool done) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (!uuid.empty() && t.uuid == uuid) {
            t.isDone      = done;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地切换待办状态 by uuid（待同步）: " + t.content);
            return;
        }
    }
}

void ApiUpdateTodo(const std::wstring &uuid, const std::wstring &content,
                   const std::wstring &createdDate, const std::wstring &dueDate, bool isDone,
                   const std::wstring &remark) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (!uuid.empty() && t.uuid == uuid) {
            t.content     = content;
            t.createdDate = createdDate;
            t.dueDate     = dueDate;
            t.isDone      = isDone;
            t.remark      = remark;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地更新待办（待同步）: " + content);
            return;
        }
    }
    LogMessage(L"ApiUpdateTodo: uuid=" + uuid + L" 未找到，降级为新增");
    ApiAddTodo(content, createdDate, dueDate, isDone, remark);
}

void ApiDeleteTodo(int id) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &t : g_Todos) {
        if (t.id == id) {
            t.isDeleted   = true;
            t.lastUpdated = time(nullptr);
            t.isDirty     = true;
            LogMessage(L"本地标记待办删除（待同步）: " + t.content);
            return;
        }
    }
    // 也尝试通过 uuid 匹配（id 可能已被服务器更新）
    LogMessage(L"ApiDeleteTodo: id=" + std::to_wstring(id) + L" 未找到");
}

void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    Countdown c;
    c.id          = -(int)(g_Countdowns.size() + 1);
    c.uuid        = GenLocalUuid();
    c.title       = title;
    c.dateStr     = dateStr;
    c.daysLeft    = CalculateDaysLeft(dateStr);
    c.lastUpdated = time(nullptr);
    c.isDirty     = true;
    g_Countdowns.push_back(c);
    LogMessage(L"本地新增倒计时（待同步）: " + title);
}

void ApiDeleteCountdown(int id) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    for (auto &c : g_Countdowns) {
        if (c.id == id) {
            c.lastUpdated = time(nullptr);
            c.isDirty     = true;
            c.id          = -(abs(c.id)); // 负数 = 标记为待删除
            LogMessage(L"本地标记倒计时删除（待同步）: " + c.title);
            break;
        }
    }
}

/**
 * 🚀 模块 C：Delta Sync 核心 —— /api/sync (POST)
 */
void SyncData() {
    if (g_UserId <= 0 || g_AuthToken.empty()) {
        LogMessage(L"SyncData 跳过：未登录或无 Token");
        return;
    }

    // 🛡️ 安全读取 JSON 字段：字段不存在或为 null 时返回默认值，避免 type_error.302
    auto safeInt = [](const json& j, const std::string& key, int def = 0) -> int {
        if (!j.contains(key) || j[key].is_null()) return def;
        if (j[key].is_number()) return j[key].get<int>();
        return def;
    };
    auto safeMs = [](const json& j, const std::string& key) -> long long {
        if (!j.contains(key) || j[key].is_null()) return 0LL;
        if (j[key].is_number()) return j[key].get<long long>();
        return 0LL;
    };
    // 支持两个备选 key（手机端用 camelCase，PC 端用 snake_case）
    auto safeMsAlt = [&safeMs](const json& j, const std::string& k1, const std::string& k2) -> long long {
        long long v = safeMs(j, k1);
        return (v != 0LL) ? v : safeMs(j, k2);
    };

    LogMessage(L"开始 Delta Sync (last_sync_time=" + std::to_wstring(g_LastSyncTime) + L")...");

    // ── 1. 构建本地变更包（dirty items）──
    json todosChanges   = json::array();
    json countdownsChanges = json::array();

    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

        for (const auto &t : g_Todos) {
            if (!t.isDirty) continue;

            json item;
            item["uuid"]         = ToUtf8(t.uuid);
            item["content"]      = ToUtf8(t.content);
            item["is_completed"] = t.isDone ? 1 : 0;
            item["is_deleted"]   = t.isDeleted ? 1 : 0;
            item["updated_at"]   = (long long)t.lastUpdated * 1000LL;
            // version 用 lastUpdated 秒数低16位，保证每次修改后递增
            item["version"]      = (int)((t.lastUpdated & 0xFFFF) + 1);

            // 日期字段：转为 UTC ms 时间戳
            long long createdMs = DateStringToUtcMs(t.createdDate);
            long long dueMs     = DateStringToUtcMs(t.dueDate);
            if (createdMs > 0) item["created_date"] = createdMs;
            else               item["created_date"] = nullptr;
            if (dueMs > 0)     item["due_date"]     = dueMs;
            else               item["due_date"]     = nullptr;

            item["created_at"] = createdMs > 0 ? createdMs : (long long)t.lastUpdated * 1000LL;

            // 循环字段
            item["recurrence"]          = t.recurrence;
            if (t.customIntervalDays > 0) item["custom_interval_days"] = t.customIntervalDays;
            else                          item["custom_interval_days"] = nullptr;
            long long recEndMs = DateStringToUtcMs(t.recurrenceEndDate);
            if (recEndMs > 0) item["recurrence_end_date"] = recEndMs;
            else              item["recurrence_end_date"] = nullptr;

            // 备注字段
            if (!t.remark.empty()) item["remark"] = ToUtf8(t.remark);
            else                   item["remark"] = nullptr;

            todosChanges.push_back(item);
        }

        for (const auto &c : g_Countdowns) {
            if (!c.isDirty) continue;

            bool isDeleted = (c.id < 0 && !c.uuid.empty());
            json item;
            item["uuid"]       = ToUtf8(c.uuid);
            item["title"]      = ToUtf8(c.title);
            item["is_deleted"] = isDeleted ? 1 : 0;
            item["updated_at"] = (long long)c.lastUpdated * 1000LL;
            item["version"]    = 1;
            item["created_at"] = (long long)c.lastUpdated * 1000LL;

            // target_time：把日期字符串转为 UTC ms
            long long targetMs = DateStringToUtcMs(c.dateStr);
            if (targetMs > 0) item["target_time"] = targetMs;
            else              item["target_time"] = nullptr;

            countdownsChanges.push_back(item);
        }
    }

    // ── 2. 屏幕时间（直接从 Tai DB 实时快照上传，不依赖 g_AppUsage）──
    json screenTimePayload = nullptr;
    {
        auto localUsage = GetLocalAppUsageMapCopy();
        if (!localUsage.empty()) {
            json apps = json::array();
            for (const auto &p : localUsage) {
                json a;
                a["app_name"] = ToUtf8(p.first);
                a["duration"] = p.second;
                apps.push_back(a);
            }
            screenTimePayload = json::object();
            screenTimePayload["device_name"] = ToUtf8(g_DeviceName);
            screenTimePayload["record_date"] = ToUtf8(GetTodayDate());
            screenTimePayload["apps"]        = apps;
        }
    }

    // ── 3. 组装完整 Payload ──
    json payload;
    payload["user_id"]        = g_UserId;
    payload["device_id"]      = ToUtf8(g_DeviceId);
    payload["last_sync_time"] = g_LastSyncTime;
    payload["todos"]          = todosChanges;
    payload["countdowns"]     = countdownsChanges;
    if (!screenTimePayload.is_null()) {
        payload["screen_time"] = screenTimePayload;
    }

    // ── 4. 发送请求 ──
    std::string res = SendRequest(L"/api/sync", "POST", payload.dump());

    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"Delta Sync 失败: " + ToWide(res));
        return;
    }

    // 🔍 调试：打印前1000字符的响应
    std::string resPreview = res.length() > 1000 ? res.substr(0, 1000) + "..." : res;
    LogMessage(L"[DEBUG] SyncResponse: " + ToWide(resPreview));

    // ── 5. 解析响应并合并到本地 ──
    try {
        auto resp = json::parse(res);
        if (!resp.contains("success") || !resp["success"].get<bool>()) {
            std::string errMsg = resp.contains("error") ? resp["error"].get<std::string>() : "unknown";
            LogMessage(L"Delta Sync 服务器拒绝: " + ToWide(errMsg));
            return;
        }

        // 更新同步限额状态
        if (resp.contains("status") && resp["status"].is_object()) {
            auto &st = resp["status"];
            if (st.contains("tier") && st["tier"].is_string())
                g_UserTier  = ToWide(st["tier"].get<std::string>());
            if (st.contains("sync_count") && st["sync_count"].is_number())
                g_SyncCount = st["sync_count"].get<int>();
            if (st.contains("sync_limit") && st["sync_limit"].is_number())
                g_SyncLimit = st["sync_limit"].get<int>();
            SaveSyncStatusToLocal();
        }

        // 更新并持久化 last_sync_time
        if (resp.contains("new_sync_time") && !resp["new_sync_time"].is_null()
            && resp["new_sync_time"].is_number()) {
            g_LastSyncTime = resp["new_sync_time"].get<long long>();
            SaveLastSyncTime(g_LastSyncTime);
        }

        // ── 5a. 合并服务器下发的 Todos ──
        if (resp.contains("server_todos") && resp["server_todos"].is_array()) {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

            for (auto &st : resp["server_todos"]) {
                bool sDeleted = false;
                if (st.contains("is_deleted")) {
                    auto v = st["is_deleted"];
                    sDeleted = v.is_number() ? (v.get<int>() != 0) : v.get<bool>();
                }

                // 安全提取 uuid：服务器返回 uuid 字符串或 id 数字
                std::string rawUuid;
                if (st.contains("uuid") && st["uuid"].is_string())
                    rawUuid = st["uuid"].get<std::string>();
                else if (st.contains("id") && st["id"].is_number())
                    rawUuid = std::to_string(st["id"].get<int>());
                std::wstring sUuid = ToWide(rawUuid);
                if (sUuid.empty()) continue;

                // 从服务器时间戳转回日期字符串
                long long createdDateMs = safeMs(st, "created_date");
                long long dueDateMs     = safeMs(st, "due_date");

                std::wstring sCreatedDate = UtcMsToDateString(createdDateMs);
                std::wstring sDueDate     = UtcMsToDateString(dueDateMs);
                std::wstring sContent     = ToWide(st.value("content", ""));
                bool sCompleted = false;
                if (st.contains("is_completed") && !st["is_completed"].is_null()) {
                    sCompleted = st["is_completed"].is_number()
                        ? st["is_completed"].get<int>() != 0
                        : (st["is_completed"].is_boolean() ? st["is_completed"].get<bool>() : false);
                }

                // 在本地列表中查找匹配的条目
                auto it = std::find_if(g_Todos.begin(), g_Todos.end(),
                    [&](const Todo &t){ return t.uuid == sUuid; });

                if (sDeleted) {
                    if (it != g_Todos.end()) g_Todos.erase(it);
                    continue;
                }

                if (it != g_Todos.end()) {
                    if (!it->isDirty) {
                        it->content     = sContent;
                        it->isDone      = sCompleted;
                        it->createdDate = sCreatedDate;
                        it->dueDate     = sDueDate;
                        it->recurrence         = safeInt(st, "recurrence");
                        it->customIntervalDays = safeInt(st, "customIntervalDays") != 0
                            ? safeInt(st, "customIntervalDays") : safeInt(st, "custom_interval_days");
                        it->recurrenceEndDate  = UtcMsToDateString(safeMsAlt(st, "recurrenceEndDate", "recurrence_end_date"));
                        // remark
                        if (st.contains("remark") && st["remark"].is_string())
                            it->remark = ToWide(st["remark"].get<std::string>());
                        else
                            it->remark = L"";
                    }
                } else {
                    Todo t;
                    t.id          = safeInt(st, "id");
                    t.uuid        = sUuid;
                    t.content     = sContent;
                    t.isDone      = sCompleted;
                    t.createdDate = sCreatedDate;
                    t.dueDate     = sDueDate;
                    t.recurrence         = safeInt(st, "recurrence");
                    t.customIntervalDays = safeInt(st, "customIntervalDays") != 0
                        ? safeInt(st, "customIntervalDays") : safeInt(st, "custom_interval_days");
                    t.recurrenceEndDate  = UtcMsToDateString(safeMsAlt(st, "recurrenceEndDate", "recurrence_end_date"));
                    // remark
                    if (st.contains("remark") && st["remark"].is_string())
                        t.remark = ToWide(st["remark"].get<std::string>());
                    t.lastUpdated = time(nullptr);
                    t.isDirty     = false;
                    g_Todos.push_back(t);
                }
            }

            // 清除已成功上传的 isDeleted 条目；同时清除未成功分配服务器 id 的临时本地条目
            g_Todos.erase(std::remove_if(g_Todos.begin(), g_Todos.end(),
                [](const Todo &t){ return t.isDeleted; }), g_Todos.end());
            for (auto &t : g_Todos) t.isDirty = false;

            LogMessage(L"Todos 合并完成，当前共 " + std::to_wstring(g_Todos.size()) + L" 条");
        }

        // ── 5b. 合并服务器下发的 Countdowns ──
        if (resp.contains("server_countdowns") && resp["server_countdowns"].is_array()) {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

            for (auto &sc : resp["server_countdowns"]) {
                bool sDeleted = false;
                if (sc.contains("is_deleted")) {
                    auto v = sc["is_deleted"];
                    sDeleted = v.is_number() ? (v.get<int>() != 0) : v.get<bool>();
                }

                // 安全提取 uuid
                std::string rawCUuid;
                if (sc.contains("uuid") && sc["uuid"].is_string())
                    rawCUuid = sc["uuid"].get<std::string>();
                else if (sc.contains("id") && sc["id"].is_number())
                    rawCUuid = std::to_string(sc["id"].get<int>());
                std::wstring sUuid  = ToWide(rawCUuid);
                std::wstring sTitle = ToWide(sc.contains("title") && !sc["title"].is_null()
                    ? sc["title"].get<std::string>() : std::string(""));
                long long targetMs  = safeMs(sc, "target_time");
                std::wstring sDateStr = UtcMsToDateOnly(targetMs);

                auto it = std::find_if(g_Countdowns.begin(), g_Countdowns.end(),
                    [&](const Countdown &c){ return c.uuid == sUuid; });

                if (sDeleted) {
                    if (it != g_Countdowns.end()) g_Countdowns.erase(it);
                    continue;
                }

                if (it != g_Countdowns.end()) {
                    if (!it->isDirty) {
                        it->title     = sTitle;
                        it->dateStr   = sDateStr;
                        it->daysLeft  = CalculateDaysLeft(sDateStr);
                    }
                } else {
                    Countdown c;
                    c.id          = sc.contains("id") && sc["id"].is_number()
                                    ? sc["id"].get<int>() : 0;
                    c.uuid        = sUuid;
                    c.title       = sTitle;
                    c.dateStr     = sDateStr;
                    c.daysLeft    = CalculateDaysLeft(sDateStr);
                    c.lastUpdated = time(nullptr);
                    c.isDirty     = false;
                    g_Countdowns.push_back(c);
                }
            }

            g_Countdowns.erase(std::remove_if(g_Countdowns.begin(), g_Countdowns.end(),
                [](const Countdown &c){ return c.id < 0; }), g_Countdowns.end());
            for (auto &c : g_Countdowns) c.isDirty = false;

            LogMessage(L"Countdowns 合并完成，当前共 " + std::to_wstring(g_Countdowns.size()) + L" 条");
        }

        // ── 5c. 同步后拉取聚合屏幕时间（仅 GET，用于展示多端数据）──
        {
            std::wstring today    = GetTodayDate();
            std::wstring getUrl   = L"/api/screen_time?user_id=" + std::to_wstring(g_UserId)
                                  + L"&date=" + today;
            std::string stRes = SendRequest(getUrl, "GET", "");
            if (!stRes.empty() && stRes.find("ERROR") != 0) {
                try {
                    auto jArr = json::parse(stRes);
                    if (jArr.is_array()) {
                        std::vector<AppUsageRecord> details;
                        for (const auto &item : jArr) {
                            AppUsageRecord r;
                            r.appName    = ToWide(item.value("app_name", ""));
                            r.deviceName = ToWide(item.value("device_name", ""));
                            r.seconds    = item.value("duration", 0);
                            details.push_back(r);
                        }
                        // 🚀 本机条目以 Tai DB 实时数据为准：
                        // 先移除服务器返回的本机条目（可能是上次同步时的旧值），
                        // 再用本机最新快照替换，其他设备条目保持服务器值不变
                        auto localSnapshot = GetLocalAppUsageMapCopy();
                        details.erase(
                            std::remove_if(details.begin(), details.end(),
                                [](const AppUsageRecord& r){ return r.deviceName == g_DeviceName; }),
                            details.end());
                        for (const auto& p : localSnapshot) {
                            details.push_back({p.first, g_DeviceName, p.second});
                        }
                        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                        g_AppUsage = details;
                    }
                } catch (...) {}
            }
        }

        LogMessage(L"Delta Sync 完成!");
        // 🚀 合并完成后立即持久化到本地，下次重启可直接加载
        SaveLocalData();
        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);

        // 🚀 自愈：若合并后本地为空但 last_sync_time > 0，
        //         说明时间戳超过了所有服务器数据的 updated_at，自动重置并重试一次全量拉取
        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            if (g_Todos.empty() && g_Countdowns.empty() && g_LastSyncTime > 0) {
                LogMessage(L"[自愈] 本地数据为空且 last_sync_time>0，重置为 0 后重试全量拉取...");
                g_LastSyncTime = 0;
                SaveLastSyncTime(0);
                // 异步重试，避免递归调用栈溢出
                std::thread([]() { SyncData(); }).detach();
            }
        }

    } catch (const std::exception &e) {
        LogMessage(L"Delta Sync 解析异常: " + ToWide(std::string(e.what())));
    } catch (...) {
        LogMessage(L"Delta Sync 未知异常");
    }
}

/**
 * 模块 D：ApiFetchUserStatus（保留兼容，用于右键菜单离线检测）
 * 注意：新后端已无 /api/user/status 端点，此函数改为从 SyncData 的
 *       status 字段更新，直接返回 true 即可（右键菜单离线检测降级处理）
 */
bool ApiFetchUserStatus() {
    // 新架构中 status 由 SyncData 响应携带更新，此函数保留以兼容调用方
    // 这里做一次轻量 SyncData（传空变更包，仅获取 status）
    if (g_UserId <= 0 || g_AuthToken.empty()) return false;

    json payload;
    payload["user_id"]        = g_UserId;
    payload["device_id"]      = ToUtf8(g_DeviceId);
    payload["last_sync_time"] = g_LastSyncTime;
    payload["todos"]          = json::array();
    payload["countdowns"]     = json::array();

    std::string res = SendRequest(L"/api/sync", "POST", payload.dump());
    if (res.empty() || res.find("ERROR") == 0) return false;

    try {
        auto resp = json::parse(res);
        if (resp.contains("status")) {
            auto &st = resp["status"];
            if (st.contains("tier"))       g_UserTier  = ToWide(st["tier"].get<std::string>());
            if (st.contains("sync_count")) g_SyncCount = st["sync_count"].get<int>();
            if (st.contains("sync_limit")) g_SyncLimit = st["sync_limit"].get<int>();
            SaveSyncStatusToLocal();
        }
        return true;
    } catch (...) { return false; }
}

/**
 * 模块 E：课程表（逻辑不变，但请求头已自动升级为 Bearer Token）
 */
void ApiFetchCourses() {
    if (g_UserId <= 0) return;
    LogMessage(L"开始从云端拉取最新课程表...");
    std::wstring url = L"/api/courses?user_id=" + std::to_wstring(g_UserId);
    std::string res = SendRequest(url, "GET", "");

    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"课程表拉取失败: 网络异常");
        return;
    }

    try {
        auto jArr = json::parse(res);
        if (jArr.is_array()) {
            std::vector<Course> tempCourses;
            for (auto &it : jArr) {
                Course c;
                c.id          = it.value("id", 0);
                c.courseName  = ToWide(it.value("course_name", "未知课程"));
                c.roomName    = ToWide(it.value("room_name",   "未知教室"));
                c.teacherName = ToWide(it.value("teacher_name", ""));
                c.startTime   = it.value("start_time", 0);
                c.endTime     = it.value("end_time",   0);
                c.weekday     = it.value("weekday",    1);
                c.weekIndex   = it.value("week_index", 1);
                if (it.contains("lesson_type") && !it["lesson_type"].is_null())
                    c.lessonType = ToWide(it.value("lesson_type", ""));
                tempCourses.push_back(c);
            }
            {
                std::lock_guard<std::recursive_mutex> l(g_DataMutex);
                g_Courses = tempCourses;
            }
            SaveLocalCourses();
            LogMessage(L"课程表拉取并缓存成功! 共 " + std::to_wstring(tempCourses.size()) + L" 节课");
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
        } else if (jArr.is_object() && jArr.contains("error")) {
            LogMessage(L"课程表拉取被拒绝: " + ToWide(jArr["error"].get<std::string>()));
        }
    } catch (...) {
        LogMessage(L"课程表数据解析 JSON 失败");
    }
}

/**
 * 兼容旧接口：ApiSyncScreenTime（由 tai_reader 调用，保留签名）
 */
std::map<std::wstring, int> ApiSyncScreenTime(
    const std::map<std::wstring, int> &localData,
    const std::wstring &dateStr,
    const std::wstring &deviceName)
{
    return localData;
}

// ============================================================
// 🍅 番茄钟 API 实现
// ============================================================

static std::wstring GetPomodoroSessionSection() { return L"Pomodoro"; }

void SavePomodoroSession() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    const std::wstring &sec = GetPomodoroSessionSection();
    auto &s = g_PomodoroSession;
    WritePrivateProfileStringW(sec.c_str(), L"Status",        std::to_wstring((int)s.status).c_str(),        path);
    WritePrivateProfileStringW(sec.c_str(), L"TargetEndMs",   std::to_wstring(s.targetEndMs).c_str(),        path);
    WritePrivateProfileStringW(sec.c_str(), L"FocusDuration", std::to_wstring(s.focusDuration).c_str(),      path);
    WritePrivateProfileStringW(sec.c_str(), L"RestDuration",  std::to_wstring(s.restDuration).c_str(),       path);
    WritePrivateProfileStringW(sec.c_str(), L"LoopCount",     std::to_wstring(s.loopCount).c_str(),          path);
    WritePrivateProfileStringW(sec.c_str(), L"CurrentLoop",   std::to_wstring(s.currentLoop).c_str(),        path);
    WritePrivateProfileStringW(sec.c_str(), L"BoundTodoUuid", s.boundTodoUuid.c_str(),                       path);
    WritePrivateProfileStringW(sec.c_str(), L"RecordUuid",    s.currentRecordUuid.c_str(),                   path);
    WritePrivateProfileStringW(sec.c_str(), L"IsRestPhase",   s.isRestPhase ? L"1" : L"0",                  path);
}

void LoadPomodoroSession() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());

    const std::wstring &sec = GetPomodoroSessionSection();
    auto readInt  = [&](const wchar_t* key, int def) -> int {
        return GetPrivateProfileIntW(sec.c_str(), key, def, path);
    };
    auto readStr  = [&](const wchar_t* key) -> std::wstring {
        WCHAR buf[512] = {};
        GetPrivateProfileStringW(sec.c_str(), key, L"", buf, 512, path);
        return buf;
    };

    auto &s = g_PomodoroSession;
    s.status         = (PomodoroStatus)readInt(L"Status",        0);
    s.targetEndMs    = 0;
    {
        std::wstring v = readStr(L"TargetEndMs");
        if (!v.empty()) {
            try { s.targetEndMs = std::stoll(v); } catch (...) {}
        }
    }
    s.focusDuration  = readInt(L"FocusDuration", 25 * 60);
    s.restDuration   = readInt(L"RestDuration",   5 * 60);
    s.loopCount      = readInt(L"LoopCount",      4);
    s.currentLoop    = readInt(L"CurrentLoop",    0);
    s.boundTodoUuid  = readStr(L"BoundTodoUuid");
    s.currentRecordUuid = readStr(L"RecordUuid");
    s.isRestPhase    = (readInt(L"IsRestPhase",   0) != 0);

    // 超时检测：如果目标结束时间已过，重置为 Idle
    if ((s.status == PomodoroStatus::Focusing || s.status == PomodoroStatus::Resting)
        && s.targetEndMs > 0) {
        long long nowMs = (long long)time(nullptr) * 1000LL;
        if (nowMs >= s.targetEndMs) {
            LogMessage(L"[番茄钟] 重启时检测到计时已结束，自动重置为 Idle");
            s.status      = PomodoroStatus::Idle;
            s.targetEndMs = 0;
            s.currentRecordUuid.clear();
            SavePomodoroSession();
        }
    }
    LogMessage(L"[番茄钟] Session 已加载，Status=" + std::to_wstring((int)s.status));
}

void ApiFetchPomodoroTags() {
    if (g_UserId <= 0 || g_AuthToken.empty()) return;
    std::string res = SendRequest(L"/api/pomodoro/tags", "GET", "");
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"[番茄钟] 拉取标签失败");
        return;
    }
    try {
        auto jArr = json::parse(res);
        if (!jArr.is_array()) {
            LogMessage(L"[番茄钟] 拉取标签响应非数组");
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_PomodoroTags.clear();
        for (const auto &j : jArr) {
            if (!j.is_object()) continue;
            PomodoroTag tag;
            // 安全读取（防止字段为 null）
            auto safeStr = [&](const char* key, const std::string& def = "") -> std::string {
                if (!j.contains(key)) return def;
                const auto& v = j[key];
                return (v.is_null() || !v.is_string()) ? def : v.get<std::string>();
            };
            auto safeInt = [&](const char* key, int def = 0) -> int {
                if (!j.contains(key)) return def;
                const auto& v = j[key];
                return (v.is_null() || !v.is_number()) ? def : v.get<int>();
            };
            tag.uuid      = ToWide(safeStr("uuid"));
            tag.name      = ToWide(safeStr("name"));
            tag.color     = ToWide(safeStr("color", "#607D8B"));
            tag.isDeleted = safeInt("is_deleted") != 0;
            tag.version   = safeInt("version", 1);
            if (j.contains("created_at") && j["created_at"].is_number())
                tag.createdAt = j["created_at"].get<long long>();
            if (j.contains("updated_at") && j["updated_at"].is_number())
                tag.updatedAt = j["updated_at"].get<long long>();
            if (!tag.uuid.empty())
                g_PomodoroTags.push_back(tag);
        }
        LogMessage(L"[番茄钟] 拉取标签成功，共 " + std::to_wstring(g_PomodoroTags.size()) + L" 个");
    } catch (const std::exception& e) {
        LogMessage(L"[番茄钟] 拉取标签 JSON 解析失败: " + ToWide(e.what()));
    } catch (...) {
        LogMessage(L"[番茄钟] 拉取标签 JSON 解析失败（未知异常）");
    }
}

void ApiSyncPomodoroTags() {
    if (g_UserId <= 0 || g_AuthToken.empty()) return;

    json tagsArr = json::array();
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        for (const auto &tag : g_PomodoroTags) {
            if (!tag.isDirty) continue;
            json j;
            j["uuid"]       = ToUtf8(tag.uuid);
            j["name"]       = ToUtf8(tag.name);
            j["color"]      = ToUtf8(tag.color);
            j["is_deleted"] = tag.isDeleted ? 1 : 0;
            j["version"]    = tag.version;
            j["created_at"] = tag.createdAt;
            j["updated_at"] = tag.updatedAt;
            tagsArr.push_back(j);
        }
    }
    if (tagsArr.empty()) return;

    json payload;
    payload["tags"] = tagsArr;
    std::string res = SendRequest(L"/api/pomodoro/tags", "POST", payload.dump());
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"[番茄钟] 同步标签失败");
        return;
    }
    try {
        auto resp = json::parse(res);
        if (resp.contains("success") && resp["success"].get<bool>()) {
            // 若返回最新标签列表，直接替换本地
            if (resp.contains("tags") && resp["tags"].is_array()) {
                std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                g_PomodoroTags.clear();
                for (const auto &j : resp["tags"]) {
                    PomodoroTag tag;
                    tag.uuid      = ToWide(j.value("uuid",  ""));
                    tag.name      = ToWide(j.value("name",  ""));
                    tag.color     = ToWide(j.value("color", "#607D8B"));
                    tag.isDeleted = j.value("is_deleted", 0) != 0;
                    tag.version   = j.value("version",    1);
                    if (j.contains("created_at") && j["created_at"].is_number())
                        tag.createdAt = j["created_at"].get<long long>();
                    if (j.contains("updated_at") && j["updated_at"].is_number())
                        tag.updatedAt = j["updated_at"].get<long long>();
                    if (!tag.uuid.empty())
                        g_PomodoroTags.push_back(tag);
                }
            }
            LogMessage(L"[番茄钟] 标签同步成功");
        }
    } catch (...) {
        LogMessage(L"[番茄钟] 同步标签响应解析失败");
    }
}

bool ApiUploadPomodoroRecord(const PomodoroRecord &rec) {
    if (g_UserId <= 0 || g_AuthToken.empty()) return false;

    json r;
    r["uuid"]             = ToUtf8(rec.uuid);
    r["todo_uuid"]        = rec.todoUuid.empty() ? json(nullptr) : json(ToUtf8(rec.todoUuid));
    r["start_time"]       = rec.startTime;
    r["end_time"]         = (rec.endTime > 0) ? json(rec.endTime) : json(nullptr);
    r["planned_duration"] = rec.plannedDuration;
    r["actual_duration"]  = rec.actualDuration;
    r["status"]           = ToUtf8(rec.status);
    r["device_id"]        = ToUtf8(g_DeviceId);
    r["is_deleted"]       = rec.isDeleted ? 1 : 0;
    r["version"]          = rec.version;
    r["created_at"]       = rec.createdAt;
    r["updated_at"]       = rec.updatedAt;
    // 🚀 上传关联标签 uuid 列表
    json tagArr = json::array();
    for (const auto& tu : rec.tagUuids)
        tagArr.push_back(ToUtf8(tu));
    r["tag_uuids"] = tagArr;

    json payload;
    payload["record"] = r;
    std::string res = SendRequest(L"/api/pomodoro/records", "POST", payload.dump());
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"[番茄钟] 上传记录失败: " + ToWide(res));
        return false;
    }
    try {
        auto resp = json::parse(res);
        bool ok = resp.contains("success") && resp["success"].get<bool>();
        if (ok) LogMessage(L"[番茄钟] 记录上传成功: " + rec.uuid);
        return ok;
    } catch (...) { return false; }
}

void ApiFetchPomodoroHistory(long long fromMs, long long toMs) {
    if (g_UserId <= 0 || g_AuthToken.empty()) return;
    std::wstring url = L"/api/pomodoro/records?from=" + std::to_wstring(fromMs)
                     + L"&to=" + std::to_wstring(toMs);
    std::string res = SendRequest(url, "GET", "");
    if (res.empty() || res.find("ERROR") == 0) {
        LogMessage(L"[番茄钟] 拉取历史记录失败");
        return;
    }
    try {
        auto jArr = json::parse(res);
        if (!jArr.is_array()) {
            // 可能是 {"error":"..."} 格式的错误响应
            LogMessage(L"[番茄钟] 历史记录响应非数组，raw=" + ToWide(res.substr(0, 200)));
            return;
        }
        // 安全读取可能为 null 的字符串字段
        auto safeStr = [](const json& j, const char* key, const std::string& def = "") -> std::string {
            if (!j.contains(key)) return def;
            const auto& v = j[key];
            if (v.is_null() || !v.is_string()) return def;
            return v.get<std::string>();
        };
        // 安全读取可能为 null 的整数字段
        auto safeInt = [](const json& j, const char* key, int def = 0) -> int {
            if (!j.contains(key)) return def;
            const auto& v = j[key];
            if (v.is_null()) return def;
            if (v.is_number()) return v.get<int>();
            return def;
        };
        // 安全读取可能为 null 的 long long 字段
        auto safeLL = [](const json& j, const char* key, long long def = 0LL) -> long long {
            if (!j.contains(key)) return def;
            const auto& v = j[key];
            if (v.is_null()) return def;
            if (v.is_number()) return v.get<long long>();
            return def;
        };

        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_PomodoroHistory.clear();
        for (const auto &j : jArr) {
            if (!j.is_object()) continue;
            PomodoroRecord rec;
            rec.uuid            = ToWide(safeStr(j, "uuid"));
            rec.todoUuid        = ToWide(safeStr(j, "todo_uuid"));
            rec.todoTitle       = ToWide(safeStr(j, "todo_title")); // 🚀 新字段
            rec.startTime       = safeLL(j, "start_time");
            rec.endTime         = safeLL(j, "end_time");
            rec.plannedDuration = safeInt(j, "planned_duration", 1500);
            rec.actualDuration  = safeInt(j, "actual_duration",  0);
            rec.status          = ToWide(safeStr(j, "status", "completed"));
            rec.deviceId        = ToWide(safeStr(j, "device_id"));
            rec.isDeleted       = safeInt(j, "is_deleted") != 0;
            rec.version         = safeInt(j, "version", 1);
            rec.createdAt       = safeLL(j, "created_at");
            rec.updatedAt       = safeLL(j, "updated_at");
            // 🚀 解析 tag_uuids 数组
            if (j.contains("tag_uuids") && j["tag_uuids"].is_array()) {
                for (const auto& tu : j["tag_uuids"]) {
                    if (tu.is_string()) rec.tagUuids.push_back(ToWide(tu.get<std::string>()));
                }
            }
            if (!rec.uuid.empty())
                g_PomodoroHistory.push_back(rec);
        }
        LogMessage(L"[番茄钟] 历史记录已加载，共 " + std::to_wstring(g_PomodoroHistory.size()) + L" 条");
    } catch (const std::exception& e) {
        LogMessage(L"[番茄钟] 历史记录 JSON 解析失败: " + ToWide(e.what()));
    } catch (...) {
        LogMessage(L"[番茄钟] 历史记录 JSON 解析失败（未知异常）");
    }
}

// ============================================================
// 🍅 番茄钟本地缓存  (pomodoro_cache.json)
// ============================================================

static std::wstring GetPomodoroCachePath() {
    WCHAR exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);
    PathAppendW(exeDir, L"pomodoro_cache.json");
    return exeDir;
}

void SavePomodoroLocalCache() {
    try {
        json root;
        root["version"]  = 1;
        root["saved_at"] = (long long)time(nullptr) * 1000LL;
        root["user_id"]  = g_UserId;

        json recsArr = json::array();
        {
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            for (const auto& rec : g_PomodoroHistory) {
                json j;
                j["uuid"]             = ToUtf8(rec.uuid);
                j["todo_uuid"]        = ToUtf8(rec.todoUuid);
                j["todo_title"]       = ToUtf8(rec.todoTitle); // 🚀
                j["start_time"]       = rec.startTime;
                j["end_time"]         = rec.endTime;
                j["planned_duration"] = rec.plannedDuration;
                j["actual_duration"]  = rec.actualDuration;
                j["status"]           = ToUtf8(rec.status);
                j["device_id"]        = ToUtf8(rec.deviceId);
                j["is_deleted"]       = rec.isDeleted ? 1 : 0;
                j["version"]          = rec.version;
                j["created_at"]       = rec.createdAt;
                j["updated_at"]       = rec.updatedAt;
                j["is_dirty"]         = rec.isDirty;
                // 🚀 持久化 tag_uuids
                json tagArr = json::array();
                for (const auto& tu : rec.tagUuids)
                    tagArr.push_back(ToUtf8(tu));
                j["tag_uuids"] = tagArr;
                recsArr.push_back(j);
            }
        }
        root["records"] = recsArr;

        std::string s = root.dump();
        std::wstring path = GetPomodoroCachePath();
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hFile, s.c_str(), (DWORD)s.size(), &written, NULL);
            CloseHandle(hFile);
            LogMessage(L"[番茄钟] 本地缓存已保存，共 " + std::to_wstring(recsArr.size()) + L" 条记录");
        }
    } catch (const std::exception& e) {
        LogMessage(L"[番茄钟] 本地缓存保存失败: " + ToWide(e.what()));
    } catch (...) {
        LogMessage(L"[番茄钟] 本地缓存保存失败（未知异常）");
    }
}

void LoadPomodoroLocalCache() {
    std::wstring path = GetPomodoroCachePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogMessage(L"[番茄钟] 无本地缓存文件，跳过加载");
        return;
    }
    DWORD sz = GetFileSize(hFile, NULL);
    std::string s(sz, '\0');
    DWORD read = 0;
    ReadFile(hFile, &s[0], sz, &read, NULL);
    CloseHandle(hFile);

    try {
        auto root = json::parse(s);
        int uid = root.value("user_id", 0);
        if (uid != g_UserId) {
            LogMessage(L"[番茄钟] 本地缓存 userId 不匹配，跳过");
            return;
        }
        if (!root.contains("records") || !root["records"].is_array()) return;

        auto safeStr = [](const json& j, const char* key, const std::string& def = "") -> std::string {
            if (!j.contains(key)) return def;
            const auto& v = j[key];
            return (v.is_null() || !v.is_string()) ? def : v.get<std::string>();
        };
        auto safeInt = [](const json& j, const char* key, int def = 0) -> int {
            if (!j.contains(key)) return def;
            const auto& v = j[key];
            return (v.is_null() || !v.is_number()) ? def : v.get<int>();
        };
        auto safeLL = [](const json& j, const char* key, long long def = 0LL) -> long long {
            if (!j.contains(key)) return def;
            const auto& v = j[key];
            return (v.is_null() || !v.is_number()) ? def : v.get<long long>();
        };

        std::vector<PomodoroRecord> recs;
        for (const auto& j : root["records"]) {
            if (!j.is_object()) continue;
            PomodoroRecord rec;
            rec.uuid            = ToWide(safeStr(j, "uuid"));
            rec.todoUuid        = ToWide(safeStr(j, "todo_uuid"));
            rec.todoTitle       = ToWide(safeStr(j, "todo_title")); // 🚀
            rec.startTime       = safeLL(j, "start_time");
            rec.endTime         = safeLL(j, "end_time");
            rec.plannedDuration = safeInt(j, "planned_duration", 1500);
            rec.actualDuration  = safeInt(j, "actual_duration", 0);
            rec.status          = ToWide(safeStr(j, "status", "completed"));
            rec.deviceId        = ToWide(safeStr(j, "device_id"));
            rec.isDeleted       = safeInt(j, "is_deleted") != 0;
            rec.version         = safeInt(j, "version", 1);
            rec.createdAt       = safeLL(j, "created_at");
            rec.updatedAt       = safeLL(j, "updated_at");
            rec.isDirty         = j.contains("is_dirty") && j["is_dirty"].is_boolean()
                                    ? j["is_dirty"].get<bool>() : true;
            // 🚀 加载 tag_uuids
            if (j.contains("tag_uuids") && j["tag_uuids"].is_array()) {
                for (const auto& tu : j["tag_uuids"]) {
                    if (tu.is_string()) rec.tagUuids.push_back(ToWide(tu.get<std::string>()));
                }
            }
            if (!rec.uuid.empty()) recs.push_back(rec);
        }

        {
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            g_PomodoroHistory = recs;
        }
        LogMessage(L"[番茄钟] 本地缓存加载完成，共 " + std::to_wstring(recs.size()) + L" 条记录");
    } catch (const std::exception& e) {
        LogMessage(L"[番茄钟] 本地缓存解析失败: " + ToWide(e.what()));
    } catch (...) {
        LogMessage(L"[番茄钟] 本地缓存解析失败（未知异常）");
    }
}

void UploadPendingPomodoroRecords() {
    if (g_UserId <= 0 || g_AuthToken.empty()) return;

    std::vector<PomodoroRecord> pending;
    {
        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        for (const auto& rec : g_PomodoroHistory)
            if (rec.isDirty && !rec.isDeleted) pending.push_back(rec);
    }

    if (pending.empty()) return;
    LogMessage(L"[番茄钟] 开始上传待发送记录，共 " + std::to_wstring(pending.size()) + L" 条");

    int uploaded = 0;
    for (auto& rec : pending) {
        if (ApiUploadPomodoroRecord(rec)) {
            // 标记为已上传
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            for (auto& r : g_PomodoroHistory) {
                if (r.uuid == rec.uuid) { r.isDirty = false; break; }
            }
            uploaded++;
        }
    }

    if (uploaded > 0) {
        SavePomodoroLocalCache();
        LogMessage(L"[番茄钟] 成功上传 " + std::to_wstring(uploaded) + L" 条记录");
    }
}

