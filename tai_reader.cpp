#include "tai_reader.h"
#include "utils.h"
#include "api.h"
#include "sqlite3.h"
#include <shlobj.h>

#pragma comment(lib, "Shell32.lib")

// 定义在 common.h 中声明的全局变量
std::wstring g_TaiDbPath;

static std::map<std::wstring, int> g_LocalAppUsage;         // 本地 Tai 数据缓存
static std::map<std::wstring, int> g_RemoteOtherDevicesUsage; // 其他设备的累加时间缓存

static HANDLE g_hReaderThread = NULL;
static bool g_ReaderRunning = false;

// 获取 Tai 数据库路径
std::wstring GetTaiDbPath() {
    // 优先使用 UI 设置中保存的自定义路径
    if (!g_TaiDbPath.empty()) {
        return g_TaiDbPath;
    }

    // 默认路径： %LocalAppData%\Tai\Data\data.db
    WCHAR path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::wstring dbPath = path;
        dbPath += L"\\Tai\\Data\\data.db";
        return dbPath;
    }
    return L"";
}

void ReadTaiData() {
    std::wstring dbPathW = GetTaiDbPath();
    if (dbPathW.empty()) return;

    std::string dbPathUtf8 = ToUtf8(dbPathW);
    sqlite3* db;

    // 使用只读模式打开数据库，防止锁定导致 Tai 自身无法写入
    if (sqlite3_open_v2(dbPathUtf8.c_str(), &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

        // Tai 的数据结构： DailyLogModels 记录每日使用，AppModels 记录应用信息
        std::string query = "SELECT AppModels.Description, AppModels.Name, DailyLogModels.Time "
                            "FROM DailyLogModels "
                            "JOIN AppModels ON DailyLogModels.AppModelID = AppModels.ID "
                            "WHERE DailyLogModels.Date LIKE '";
        query += dateStr;
        query += "%'";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
            std::map<std::wstring, int> tempLocal;

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* desc = sqlite3_column_text(stmt, 0);
                const unsigned char* name = sqlite3_column_text(stmt, 1);
                int timeSec = sqlite3_column_int(stmt, 2);

                // 如果有中文描述（Description）则优先显示，否则使用进程名（Name）
                std::string appNameStr = "Unknown";
                if (desc && strlen((const char*)desc) > 0) {
                    appNameStr = (const char*)desc;
                } else if (name && strlen((const char*)name) > 0) {
                    appNameStr = (const char*)name;
                }

                std::wstring appNameW = ToWide(appNameStr);
                tempLocal[appNameW] += timeSec; // Tai 的 Time 字段存储的是活跃秒数
            }
            sqlite3_finalize(stmt);

            // 安全更新本地数据缓存
            {
                std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                g_LocalAppUsage = tempLocal;
            }
        }
        sqlite3_close(db);
    }
}

// 刷新内存中的合并数据：本地最新 + 远端其他设备缓存
void UpdateMergedUsage() {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    g_AppUsage = g_LocalAppUsage;
    for (const auto& o : g_RemoteOtherDevicesUsage) {
        g_AppUsage[o.first] += o.second;
    }
}

// 预留接口：从服务器拉取其他设备屏幕时间的逻辑
void SyncRemoteScreenTime() {
    if (g_UserId == 0) return;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t dateBuf[20];
    wsprintfW(dateBuf, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    // 取出当时刻的本地数据快照
    std::map<std::wstring, int> localCopy;
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        localCopy = g_LocalAppUsage;
    }

    // 调用网络接口同步，并返回包含所有设备的合计数据
    // 注意：dateBuf 是 wchar_t 数组，可以直接作为 wstring 传递
    auto aggregated = ApiSyncScreenTime(localCopy, dateBuf, g_DeviceName);

    // 剔除当前的本地数据，计算出单纯由“其他设备”贡献的时长
    std::map<std::wstring, int> others;
    for (const auto& pair : aggregated) {
        int otherVal = pair.second - localCopy[pair.first];
        if (otherVal > 0) {
            others[pair.first] = otherVal;
        }
    }

    // 更新远端缓存，并立即触发合并
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_RemoteOtherDevicesUsage = others;
    }
    UpdateMergedUsage();
}

DWORD WINAPI TaiReaderProc(LPVOID lpParam) {
    int loopCount = 0;
    while (g_ReaderRunning) {
        ReadTaiData();
        UpdateMergedUsage(); // 每 10 秒合并一次最新本地时间

        // 每 6 轮 (即约 60 秒) 进行一次真正的网络服务器同步
        if (g_UserId != 0 && loopCount % 6 == 0) {
            SyncRemoteScreenTime();
        }

        if (g_hWidgetWnd) {
            PostMessage(g_hWidgetWnd, WM_USER_TICK, 0, 0);
        }

        loopCount++;
        // 每 10 秒读取一次，分成小段睡眠以便能及时响应退出信号
        for (int i = 0; i < 10 && g_ReaderRunning; ++i) {
            Sleep(1000);
        }
    }
    return 0;
}

void StartTaiReader() {
    if (g_ReaderRunning) return;
    g_ReaderRunning = true;
    g_hReaderThread = CreateThread(NULL, 0, TaiReaderProc, NULL, 0, NULL);
}

void StopTaiReader() {
    g_ReaderRunning = false;
    if (g_hReaderThread) {
        WaitForSingleObject(g_hReaderThread, 3000);
        CloseHandle(g_hReaderThread);
        g_hReaderThread = NULL;
    }
}

std::vector<std::pair<std::wstring, int>> GetTopApps(int topN) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    std::vector<std::pair<std::wstring, int>> apps(g_AppUsage.begin(), g_AppUsage.end());

    // 按使用时间降序排列
    std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    if (apps.size() > (size_t)topN) {
        apps.resize(topN);
    }
    return apps;
}

int GetTotalScreenTime() {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    int total = 0;
    for (const auto& app : g_AppUsage) {
        total += app.second;
    }
    return total;
}