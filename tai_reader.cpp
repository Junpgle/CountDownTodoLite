#include "tai_reader.h"
#include "utils.h"
#include "api.h"
#include "sqlite3.h"
#include <shlobj.h>

#pragma comment(lib, "Shell32.lib")

// 注意：g_TaiDbPath 已在 globals.cpp 中定义，此处仅通过 common.h 引用，不再重复定义

// 内部使用的统计 Map
static std::map<std::wstring, int> g_LocalAppUsageMap;
static std::vector<AppUsageRecord> g_RemoteRecords;

static HANDLE g_hReaderThread = NULL;
static bool g_ReaderRunning = false;

// 获取 Tai 数据库路径
std::wstring GetTaiDbPath() {
    if (!g_TaiDbPath.empty()) return g_TaiDbPath;
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

    if (sqlite3_open_v2(dbPathUtf8.c_str(), &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        SYSTEMTIME st; GetLocalTime(&st);
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

        std::string query = "SELECT AppModels.Description, AppModels.Name, DailyLogModels.Time "
                            "FROM DailyLogModels "
                            "JOIN AppModels ON DailyLogModels.AppModelID = AppModels.ID "
                            "WHERE DailyLogModels.Date LIKE '" + std::string(dateStr) + "%'";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
            std::map<std::wstring, int> tempLocal;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* desc = sqlite3_column_text(stmt, 0);
                const unsigned char* name = sqlite3_column_text(stmt, 1);
                int timeSec = sqlite3_column_int(stmt, 2);

                std::string appNameStr = "Unknown";
                if (desc && strlen((const char*)desc) > 0) appNameStr = (const char*)desc;
                else if (name && strlen((const char*)name) > 0) appNameStr = (const char*)name;

                tempLocal[ToWide(appNameStr)] += timeSec;
            }
            sqlite3_finalize(stmt);

            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            g_LocalAppUsageMap = tempLocal;
        }
        sqlite3_close(db);
    }
}

// 合并本地和远程明细到全局 vector
// 策略：用本机实时 Tai 数据替换 g_AppUsage 中属于本机的条目
// 其他设备的条目保持不变，以服务器数据为准
// 注意：不使用 MAX 策略，直接替换，避免累积膨胀
void UpdateMergedUsage() {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

    if (g_UserId == 0) {
        // 未登录：只有本机数据，完整重建
        g_AppUsage.clear();
        for (const auto& pair : g_LocalAppUsageMap) {
            g_AppUsage.push_back({pair.first, g_DeviceName, pair.second});
        }
        return;
    }

    // 已登录：先移除旧的本机条目（服务器下发的或上次写入的），再插入最新 Tai 数据
    g_AppUsage.erase(
        std::remove_if(g_AppUsage.begin(), g_AppUsage.end(),
            [](const AppUsageRecord& r) { return r.deviceName == g_DeviceName; }),
        g_AppUsage.end());

    for (const auto& pair : g_LocalAppUsageMap) {
        g_AppUsage.push_back({pair.first, g_DeviceName, pair.second});
    }
}

// 返回本机 Tai 数据的线程安全副本，供 SyncData() 上传使用
std::map<std::wstring, int> GetLocalAppUsageMapCopy() {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    return g_LocalAppUsageMap;
}

DWORD WINAPI TaiReaderProc(LPVOID lpParam) {
    while (g_ReaderRunning) {
        // 每轮从 Tai DB 读取最新本机数据，更新 g_LocalAppUsageMap
        ReadTaiData();

        // 每轮都把本机最新数据写入 g_AppUsage 的本机条目
        // 登录/未登录均执行：UpdateMergedUsage 内部已区分两种情况
        UpdateMergedUsage();

        if (g_hWidgetWnd) PostMessage(g_hWidgetWnd, WM_USER_TICK, 0, 0);

        for (int i = 0; i < 10 && g_ReaderRunning; ++i) Sleep(1000);
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

int GetTotalScreenTime() {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    int total = 0;
    for (const auto& app : g_AppUsage) {
        total += app.seconds;
    }
    return total;
}

std::vector<std::pair<std::wstring, int>> GetTopApps(int topN) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    std::map<std::wstring, int> aggregated;
    for (const auto& rec : g_AppUsage) {
        aggregated[rec.appName] += rec.seconds;
    }

    std::vector<std::pair<std::wstring, int>> apps(aggregated.begin(), aggregated.end());
    std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    if (apps.size() > (size_t)topN) apps.resize(topN);
    return apps;
}