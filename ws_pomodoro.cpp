/**
 * ws_pomodoro.cpp
 * 番茄钟跨端 WebSocket 实时感知（WinHTTP WebSocket API）
 */
#include "ws_pomodoro.h"
#include "utils.h"
#include "api.h"
#include "pomodoro_overlay.h"
#include <winhttp.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

// ============================================================
// 内部状态
// ============================================================
static HINTERNET s_hSession  = NULL;
static HINTERNET s_hConnect  = NULL;
static HINTERNET s_hRequest  = NULL;
static HINTERNET s_hWs       = NULL;

static std::atomic<bool> s_Running{ false };
static std::atomic<bool> s_Connected{ false };
static std::mutex        s_SendMutex;

// ============================================================
// 工具：JSON 字符串转义
// ============================================================
static std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out.push_back(c);
    }
    return out;
}

// ============================================================
// 极简 JSON 字段提取
// ============================================================
static std::string ExtractJsonStr(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    auto pos = json.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        pos++;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos+1 < json.size()) { pos++; }
            val.push_back(json[pos++]);
        }
        return val;
    }
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) end = json.size();
    std::string v = json.substr(pos, end - pos);
    // trim whitespace
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    return v;
}

static long long ExtractJsonLL(const std::string& json, const char* key, long long def = 0) {
    std::string v = ExtractJsonStr(json, key);
    if (v.empty()) return def;
    try { return std::stoll(v); } catch (...) { return def; }
}
static int ExtractJsonInt(const std::string& json, const char* key, int def = 0) {
    return (int)ExtractJsonLL(json, key, def);
}
static bool ExtractJsonBool(const std::string& json, const char* key, bool def = false) {
    std::string v = ExtractJsonStr(json, key);
    if (v == "true")  return true;
    if (v == "false") return false;
    return def;
}

// 提取 JSON 字符串数组（简易实现，足以应对 tags 数组）
// 格式："tags":["name1","name2",...]
static std::vector<std::string> ExtractJsonStrArray(const std::string& json, const char* key) {
    std::vector<std::string> result;
    std::string k = std::string("\"") + key + "\"";
    auto pos = json.find(k);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + k.size());
    if (pos == std::string::npos) return result;
    auto end = json.find(']', pos);
    if (end == std::string::npos) return result;
    // 逐项解析
    size_t cur = pos + 1;
    while (cur < end) {
        while (cur < end && (json[cur] == ' ' || json[cur] == ',')) cur++;
        if (cur >= end) break;
        if (json[cur] == '"') {
            cur++;
            std::string val;
            while (cur < end && json[cur] != '"') {
                if (json[cur] == '\\' && cur+1 < end) { cur++; }
                val.push_back(json[cur++]);
            }
            if (!val.empty()) result.push_back(val);
            cur++; // skip closing "
        } else {
            cur++;
        }
    }
    return result;
}

// ============================================================
// 发送文本帧（线程安全）
// ============================================================
static bool WsSend(const std::string& msg) {
    if (!s_hWs || !s_Connected) return false;
    std::lock_guard<std::mutex> lk(s_SendMutex);
    DWORD err = WinHttpWebSocketSend(
        s_hWs,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)msg.c_str(),
        (DWORD)msg.size());
    return err == ERROR_SUCCESS;
}

// ============================================================
// 接收线程（正确处理分片）
// ============================================================
static void RecvLoop() {
    const DWORD BUFSIZE = 32768;
    std::vector<BYTE> buf(BUFSIZE);
    std::string accumulate;
    int consecutiveErrors = 0; // 连续错误计数

    while (s_Running && s_hWs) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType{};
        DWORD err = WinHttpWebSocketReceive(
            s_hWs, buf.data(), BUFSIZE, &bytesRead, &bufType);

        if (err != ERROR_SUCCESS) {
            consecutiveErrors++;
            LogMessage(L"[WS番茄钟] Receive error=" + std::to_wstring(err)
                + L" (连续" + std::to_wstring(consecutiveErrors) + L"次)");
            if (consecutiveErrors >= 3) {
                LogMessage(L"[WS番茄钟] 连续错误3次，断开重连");
                break;
            }
            Sleep(200); // 短暂等待后重试
            continue;
        }

        consecutiveErrors = 0; // 重置错误计数

        if (bytesRead > 0)
            accumulate.append((char*)buf.data(), bytesRead);

        bool isComplete =
            (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) ||
            (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE);

        if (!isComplete) continue;

        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            LogMessage(L"[WS番茄钟] 服务器关闭连接");
            accumulate.clear();
            break;
        }

        std::string msg = std::move(accumulate);
        accumulate.clear();
        if (msg.empty()) continue;

        LogMessage(L"[WS番茄钟] 收到: " + ToWide(msg.size() > 300 ? msg.substr(0, 300) + "..." : msg));

        std::string action = ExtractJsonStr(msg, "action");
        if (action.empty()) continue;
        // 🚀 忽略心跳（客户端 PING / HEARTBEAT）
        if (action == "PING" || action == "HEARTBEAT") continue;

        std::string srcDev = ExtractJsonStr(msg, "sourceDevice");
        std::wstring srcDevW = ToWide(srcDev);
        if (srcDevW == g_DeviceId) continue;

        if (action == "START" || action == "SYNC" || action == "SYNC_FOCUS" || action == "RECONNECT_SYNC") {
            // 🚀 新增：处理冲突纠正指令
            if (action == "SYNC_FOCUS") {
                LogMessage(L"[WS番茄钟] ⚠️ 收到云端纠正指令！准备覆盖本地状态");
                // 呼叫外部 UI 层：立刻杀掉本地正在运行的专注定时器！
                extern void NotifyLocalPomodoroConflict();
                NotifyLocalPomodoroConflict();
            }
            // 🚀 兼容 Flutter 端字段名（targetEndMs / target_end_ms / endTime）
            long long targetEndMs = ExtractJsonLL(msg, "targetEndMs");
            if (targetEndMs == 0) targetEndMs = ExtractJsonLL(msg, "target_end_ms");
            if (targetEndMs == 0) targetEndMs = ExtractJsonLL(msg, "endTime");

            long long startTimeMs = ExtractJsonLL(msg, "timestamp");
            if (startTimeMs == 0) startTimeMs = ExtractJsonLL(msg, "startTime");
            if (startTimeMs == 0) startTimeMs = ExtractJsonLL(msg, "start_time");

            // plannedSecs / planned_duration / duration（Flutter 端）
            int plannedSecs = ExtractJsonInt(msg, "plannedSecs", 0);
            if (plannedSecs == 0) {
                int pd = ExtractJsonInt(msg, "planned_duration", 0);
                if (pd > 0) plannedSecs = pd;
            }
            if (plannedSecs == 0) {
                int pd = ExtractJsonInt(msg, "duration", 0);
                if (pd > 0) plannedSecs = pd;
            }
            if (plannedSecs == 0) plannedSecs = 1500;

            // todo_uuid：存起来，用于本地查找 + 同步回填
            std::string tuuid = ExtractJsonStr(msg, "todo_uuid");
            if (tuuid.empty()) tuuid = ExtractJsonStr(msg, "todoUuid");
            std::wstring todoUuidW = ToWide(tuuid);

            // todoContent：优先直接字段，否则本地 g_Todos 查找
            std::wstring todoContent = ToWide(ExtractJsonStr(msg, "todoContent"));
            if (todoContent.empty()) todoContent = ToWide(ExtractJsonStr(msg, "todo_content"));
            if (todoContent.empty() && !tuuid.empty()) {
                std::lock_guard<std::recursive_mutex> lk2(g_DataMutex);
                for (const auto& t : g_Todos) {
                    if (ToUtf8(t.uuid) == tuuid) {
                        todoContent = t.content;
                        break;
                    }
                }
            }

            // isRest / is_rest
            bool isRest = ExtractJsonBool(msg, "isRest");
            if (!isRest) isRest = ExtractJsonBool(msg, "is_rest");

            // 🚀 智能兼容：不管对方发的是明文名字，还是误发的 UUID，统统处理
            auto tagNamesRaw = ExtractJsonStrArray(msg, "tags");
            std::vector<std::wstring> tagNamesW;
            for (const auto& tn : tagNamesRaw) {
                std::wstring wtn = ToWide(tn);
                // 简单判断是不是 UUID 格式 (长度 36 且包含 '-')
                if (wtn.length() == 36 && wtn.length() > 8 && wtn[8] == L'-') {
                    bool found = false;
                    std::lock_guard<std::recursive_mutex> lk_tag(g_DataMutex);
                    for (const auto& tag : g_PomodoroTags) {
                        if (tag.uuid == wtn) {
                            tagNamesW.push_back(tag.name); // 翻译成真实名字
                            found = true;
                            break;
                        }
                    }
                    if (!found) tagNamesW.push_back(L"未知标签");
                } else {
                    tagNamesW.push_back(wtn); // 本来就是明文，直接存
                }
            }

            // 推算 targetEndMs
            long long nowMs = (long long)time(nullptr) * 1000LL;
            if (targetEndMs == 0 && startTimeMs > 0)
                targetEndMs = startTimeMs + (long long)plannedSecs * 1000LL;
            if (targetEndMs == 0)
                targetEndMs = nowMs + (long long)plannedSecs * 1000LL;

            RemoteFocusState rf;
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                g_RemoteFocus.active        = true;
                g_RemoteFocus.sourceDevice  = srcDevW;
                g_RemoteFocus.todoUuid      = todoUuidW;
                g_RemoteFocus.todoContent   = todoContent;
                g_RemoteFocus.targetEndMs   = targetEndMs;
                g_RemoteFocus.startTimeMs   = startTimeMs > 0
                    ? startTimeMs
                    : (targetEndMs - (long long)plannedSecs * 1000LL);
                g_RemoteFocus.plannedSecs   = plannedSecs;
                g_RemoteFocus.isRestPhase   = isRest;
                g_RemoteFocus.receivedAt    = nowMs;
                if (!tagNamesW.empty())
                    g_RemoteFocus.tagNames  = tagNamesW;
                rf = g_RemoteFocus;   // 拷贝快照
            }
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
            // 🚀 通过 PostMessage 把快照推给主线程显示 overlay（避免跨线程 GDI）
            NotifyOverlayRemoteFocus(rf);
            LogMessage(L"[WS番茄钟] 远端专注 from=" + srcDevW
                + L" end=" + std::to_wstring(targetEndMs)
                + L" planned=" + std::to_wstring(plannedSecs)
                + L" todo=" + todoContent);

            // 🚀 若有绑定任务但本地找不到内容（未同步），触发增量同步后回填显示
            if (!tuuid.empty() && todoContent.empty()) {
                LogMessage(L"[WS番茄钟] 绑定任务本地未找到，触发增量同步回填...");
                std::thread([todoUuidW]() {
                    Sleep(300);
                    SyncData();
                    RemoteFocusState rf2;
                    bool changed = false;
                    {
                        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                        if (g_RemoteFocus.active && g_RemoteFocus.todoUuid == todoUuidW
                            && g_RemoteFocus.todoContent.empty()) {
                            for (const auto& t : g_Todos) {
                                if (t.uuid == todoUuidW) {
                                    g_RemoteFocus.todoContent = t.content;
                                    changed = true;
                                    break;
                                }
                            }
                        }
                        rf2 = g_RemoteFocus;
                    }
                    if (changed) {
                        if (g_hWidgetWnd)
                            PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
                        NotifyOverlayRemoteFocus(rf2);
                    }
                }).detach();
            }

        } else if (action == "STOP" || action == "INTERRUPT") {
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                g_RemoteFocus = RemoteFocusState{};
            }
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
            // 🚀 通过 PostMessage 通知主线程隐藏 overlay（WS线程不能直接操作窗口）
            NotifyOverlayRemoteStop();
            LogMessage(L"[WS番茄钟] 远端专注结束 from=" + srcDevW);

        } else if (action == "SWITCH") {
            // 🚀 仅更新绑定任务，保留倒计时状态不变
            std::string tuuid = ExtractJsonStr(msg, "todo_uuid");
            if (tuuid.empty()) tuuid = ExtractJsonStr(msg, "todoUuid");
            std::wstring todoUuidW = ToWide(tuuid);

            // 消息里直接带了 todo_title，优先用
            std::wstring todoContent = ToWide(ExtractJsonStr(msg, "todo_title"));
            if (todoContent.empty()) todoContent = ToWide(ExtractJsonStr(msg, "todoContent"));
            if (todoContent.empty()) todoContent = ToWide(ExtractJsonStr(msg, "todo_content"));

            // 若消息里没有标题，本地 g_Todos 查一次
            if (todoContent.empty() && !tuuid.empty()) {
                std::lock_guard<std::recursive_mutex> lk2(g_DataMutex);
                for (const auto& t : g_Todos) {
                    if (ToUtf8(t.uuid) == tuuid) {
                        todoContent = t.content;
                        break;
                    }
                }
            }

            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                if (g_RemoteFocus.active) {
                    g_RemoteFocus.todoUuid    = todoUuidW;
                    g_RemoteFocus.todoContent = todoContent;
                }
            }
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
            // 同步推给 overlay
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                if (g_RemoteFocus.active) NotifyOverlayRemoteFocus(g_RemoteFocus);
            }
            LogMessage(L"[WS番茄钟] 远端切换任务 from=" + srcDevW
                + L" uuid=" + todoUuidW + L" title=" + todoContent);

            if (!tuuid.empty() && todoContent.empty()) {
                std::thread([todoUuidW]() {
                    Sleep(300);
                    SyncData();
                    RemoteFocusState rf3;
                    bool changed = false;
                    {
                        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                        if (g_RemoteFocus.active && g_RemoteFocus.todoUuid == todoUuidW
                            && g_RemoteFocus.todoContent.empty()) {
                            for (const auto& t : g_Todos) {
                                if (t.uuid == todoUuidW) {
                                    g_RemoteFocus.todoContent = t.content;
                                    changed = true;
                                    break;
                                }
                            }
                        }
                        rf3 = g_RemoteFocus;
                    }
                    if (changed) {
                        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
                        NotifyOverlayRemoteFocus(rf3);
                    }
                }).detach();
            }

        } else if (action == "SYNC_TAGS" || action == "UPDATE_TAGS") {
            auto tagNamesRaw = ExtractJsonStrArray(msg, "tags");
            std::vector<std::wstring> tagNamesW;
            for (const auto& tn : tagNamesRaw) tagNamesW.push_back(ToWide(tn));
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                g_RemoteFocus.tagNames = tagNamesW;
                if (g_RemoteFocus.active) NotifyOverlayRemoteFocus(g_RemoteFocus);
            }
            if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_REFRESH, 0, 0);
            LogMessage(L"[WS番茄钟] 标签更新 from=" + srcDevW
                + L" count=" + std::to_wstring(tagNamesW.size()));
        }
    }

    s_Connected = false;
    LogMessage(L"[WS番茄钟] 接收线程退出");
}

// ============================================================
// 公开接口实现
// ============================================================
void WsPomodoroConnect() {
    if (s_Connected || s_Running) return;
    if (g_UserId <= 0 || g_DeviceId.empty()) return;

    s_Running   = true;
    s_Connected = false;

    std::thread([]() {
        while (s_Running && g_UserId > 0) {
            std::wstring wsPath = L"/?userId=" + std::to_wstring(g_UserId)
                                + L"&deviceId=" + g_DeviceId;

            LogMessage(L"[WS番茄钟] 正在连接 " + WS_HOST
                + L":" + std::to_wstring(WS_PORT) + wsPath);

            do {
                // 🚀 WINHTTP_FLAG_ASYNC=0，使用同步模式
                s_hSession = WinHttpOpen(L"MathQuizLite-WS/1.0",
                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                if (!s_hSession) {
                    LogMessage(L"[WS番茄钟] WinHttpOpen 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                // 🚀 只设连接超时和发送超时，不设接收超时
                // WebSocket 长连接接收是阻塞的，设接收超时会导致 12017 断连
                DWORD connTo = 10000;
                WinHttpSetOption(s_hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &connTo, sizeof(connTo));
                DWORD sendTo = 15000;
                WinHttpSetOption(s_hSession, WINHTTP_OPTION_SEND_TIMEOUT, &sendTo, sizeof(sendTo));

                s_hConnect = WinHttpConnect(s_hSession, WS_HOST.c_str(), WS_PORT, 0);
                if (!s_hConnect) {
                    LogMessage(L"[WS番茄钟] WinHttpConnect 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                // 🚀 关键：纯 HTTP（非 HTTPS），不传 WINHTTP_FLAG_SECURE
                s_hRequest = WinHttpOpenRequest(s_hConnect, L"GET", wsPath.c_str(),
                    NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                if (!s_hRequest) {
                    LogMessage(L"[WS番茄钟] WinHttpOpenRequest 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                // 🚀 在 request 上设置接收超时为最大值（无限等待）
                // 注意：0 在某些 Windows 版本会被忽略，用 MAXDWORD 更可靠
                DWORD recvTo = MAXDWORD;
                WinHttpSetOption(s_hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recvTo, sizeof(recvTo));

                // 🚀 升级 WebSocket
                if (!WinHttpSetOption(s_hRequest,
                        WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
                    LogMessage(L"[WS番茄钟] UPGRADE_TO_WEB_SOCKET 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                if (!WinHttpSendRequest(s_hRequest,
                        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                    LogMessage(L"[WS番茄钟] SendRequest 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                if (!WinHttpReceiveResponse(s_hRequest, NULL)) {
                    LogMessage(L"[WS番茄钟] ReceiveResponse 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                // 检查 HTTP 状态码（WebSocket 升级应为 101）
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(s_hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    NULL, &statusCode, &statusSize, NULL);
                LogMessage(L"[WS番茄钟] HTTP 状态码=" + std::to_wstring(statusCode));

                s_hWs = WinHttpWebSocketCompleteUpgrade(s_hRequest, 0);
                if (!s_hWs) {
                    LogMessage(L"[WS番茄钟] CompleteUpgrade 失败 err=" + std::to_wstring(GetLastError()));
                    break;
                }

                // 请求句柄用完
                WinHttpCloseHandle(s_hRequest); s_hRequest = NULL;

                // 🚀 新增：通知上层 UI 连接已恢复，可以上报本地离线专注了
                extern void NotifyWsReconnected();
                std::thread([]() {
                    Sleep(500); // 稍微延迟，等主线程反应过来
                    NotifyWsReconnected();
                }).detach();

                s_Connected = true;
                LogMessage(L"[WS番茄钟] ✅ 连接成功 userId=" + std::to_wstring(g_UserId));

                // 🚀 心跳线程：每 25 秒发一个 PING，防止 NAT 超时断连
                // 用 shared_ptr 避免 do-while 退出后 pingRunning 悬空崩溃
                auto pingRunning = std::make_shared<std::atomic<bool>>(true);
                std::thread([pingRunning]() {   // 值捕获 shared_ptr，延长生命期
                    int tick = 0;
                    while (*pingRunning && s_Connected && s_hWs) {
                        Sleep(500);
                        tick++;
                        if (tick % 50 == 0) { // 每 25s 发一次 PING
                            if (s_hWs && s_Connected && *pingRunning) {
                                std::lock_guard<std::mutex> lk(s_SendMutex);
                                WinHttpWebSocketSend(s_hWs,
                                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    (PVOID)"{\"action\":\"PING\"}", 17);
                            }
                        }
                    }
                }).detach();

                RecvLoop(); // 阻塞直到断开

                *pingRunning = false; // 通知 ping 线程退出

            } while (false);

            // ── 清理句柄（不调用 WebSocketClose，避免对已断开连接挂死）────
            if (s_hWs)      { WinHttpCloseHandle(s_hWs);     s_hWs     = NULL; }
            if (s_hRequest) { WinHttpCloseHandle(s_hRequest); s_hRequest = NULL; }
            if (s_hConnect) { WinHttpCloseHandle(s_hConnect); s_hConnect = NULL; }
            if (s_hSession) { WinHttpCloseHandle(s_hSession); s_hSession = NULL; }
            s_Connected = false;

            // 主动断开时退出重连循环
            if (!s_Running || g_UserId <= 0) {
                LogMessage(L"[WS番茄钟] 主动断开，不再重连");
                break;
            }

            LogMessage(L"[WS番茄钟] 连接断开，5秒后重连...");
            for (int i = 0; i < 50 && s_Running && g_UserId > 0; i++) Sleep(100);
            if (!s_Running || g_UserId <= 0) break;
            LogMessage(L"[WS番茄钟] 开始重连...");
            // 继续 while 循环，重新建立连接
        } // end while(s_Running)

        s_Running = false;
        LogMessage(L"[WS番茄钟] 连接线程退出");
    }).detach();
}

void WsPomodoroDisconnect() {
    s_Running   = false;
    s_Connected = false;
    if (s_hWs) {
        WinHttpWebSocketClose(s_hWs,
            WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    }
    std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
    g_RemoteFocus = RemoteFocusState{};
}

// 辅助：把 wstring 列表序列化成 JSON 字符串数组 ["a","b",...]
static std::string BuildTagsArray(const std::vector<std::wstring>& names) {
    std::string out = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ",";
        out += "\"" + JsonEscape(ToUtf8(names[i])) + "\"";
    }
    out += "]";
    return out;
}

void WsPomodoroSendStart(long long targetEndMs, int plannedSecs,
                          const std::wstring& todoContent,
                          const std::wstring& todoUuid,
                          bool isRest,
                          const std::vector<std::wstring>& tagNames)
{
    if (!s_Connected) return;
    long long nowMs = (long long)time(nullptr) * 1000LL;
    std::string content  = JsonEscape(ToUtf8(todoContent));
    std::string uuid     = JsonEscape(ToUtf8(todoUuid));
    std::string tagsJson = BuildTagsArray(tagNames);
    std::ostringstream ss;
    ss << "{"
       << "\"action\":\"START\","
       << "\"targetEndMs\":" << targetEndMs  << ","
       << "\"plannedSecs\":" << plannedSecs   << ","
       << "\"todoContent\":\"" << content    << "\","
       << "\"todo_title\":\""  << content    << "\","
       << "\"todo_uuid\":\""   << uuid       << "\","
       << "\"isRest\":"    << (isRest ? "true" : "false") << ","
       << "\"tags\":"      << tagsJson        << ","
       << "\"timestamp\":" << nowMs
       << "}";
    WsSend(ss.str());
    LogMessage(L"[WS番茄钟] 已发送 START end=" + std::to_wstring(targetEndMs)
               + L" uuid=" + todoUuid
               + L" tags=" + std::to_wstring(tagNames.size()));
}

void WsPomodoroSendUpdateTags(const std::vector<std::wstring>& tagNames) {
    if (!s_Connected) return;
    std::string tagsJson = BuildTagsArray(tagNames);
    std::string msg = "{\"action\":\"UPDATE_TAGS\",\"tags\":" + tagsJson + "}";
    WsSend(msg);
    LogMessage(L"[WS番茄钟] 已发送 UPDATE_TAGS count=" + std::to_wstring(tagNames.size()));
}

void WsPomodoroSendStop() {
    if (!s_Connected) return;
    WsSend("{\"action\":\"STOP\"}");
    LogMessage(L"[WS番茄钟] 已发送 STOP");
}

// ---------------------------------------------------------
// 🚀 新增：发送断线重连同步信号 (RECONNECT_SYNC)
// ---------------------------------------------------------
void WsPomodoroSendReconnectSync(long long targetEndMs, int plannedSecs,
                                 const std::wstring& todoContent,
                                 const std::wstring& todoUuid,
                                 bool isRest,
                                 const std::vector<std::wstring>& tagNames)
{
    if (!s_Connected) return;
    long long nowMs = (long long)time(nullptr) * 1000LL;
    std::string content  = JsonEscape(ToUtf8(todoContent));
    std::string uuid     = JsonEscape(ToUtf8(todoUuid));
    std::string tagsJson = BuildTagsArray(tagNames);

    std::ostringstream ss;
    ss << "{"
       << "\"action\":\"RECONNECT_SYNC\"," // 专属 Action
       << "\"targetEndMs\":" << targetEndMs  << ","
       << "\"plannedSecs\":" << plannedSecs   << ","
       << "\"todoContent\":\"" << content    << "\","
       << "\"todo_title\":\""  << content    << "\","
       << "\"todo_uuid\":\""   << uuid       << "\","
       << "\"isRest\":"    << (isRest ? "true" : "false") << ","
       << "\"tags\":"      << tagsJson        << ","
       << "\"timestamp\":" << nowMs
       << "}";

    WsSend(ss.str());
    LogMessage(L"[WS番茄钟] 📤 已发送离线状态同步 RECONNECT_SYNC");
}