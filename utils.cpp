#include "utils.h"

int S(int val) {
    return (int) (val * g_Scale);
}

std::string ToUtf8(const std::wstring &wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int) wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int) wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring ToWide(const std::string &str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

time_t ParseSqlTime(const std::string &s) {
    if (s.empty()) return 0;
    int y, m, d, H, M, S_time;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S_time) != 6) {
        if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d, &H, &M, &S_time) != 6) return std::time(nullptr);
    }
    std::tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = H;
    tm.tm_min = M;
    tm.tm_sec = S_time;
    tm.tm_isdst = 0;
    return _mkgmtime(&tm);
}

int CalculateDaysLeft(const std::wstring &dateStr) {
    std::string ds = ToUtf8(dateStr);
    if (ds.length() < 10) return 0;
    int y, m, d;
    if (sscanf(ds.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
    std::tm target = {0};
    target.tm_year = y - 1900;
    target.tm_mon = m - 1;
    target.tm_mday = d;
    target.tm_isdst = -1;
    std::time_t targetTime = std::mktime(&target);
    std::time_t nowTime = std::time(nullptr);
    if (targetTime == -1) return 0;
    double seconds = std::difftime(targetTime, nowTime);
    return (int) ceil(seconds / (60 * 60 * 24));
}

std::wstring EncryptString(const std::wstring &input) {
    if (input.empty()) return L"";
    DATA_BLOB DataIn = {(DWORD) ((input.length() + 1) * sizeof(wchar_t)), (BYTE *) input.c_str()};
    DATA_BLOB DataOut;
    if (CryptProtectData(&DataIn, L"MathQuizPwd", NULL, NULL, NULL, 0, &DataOut)) {
        std::wstringstream ss;
        for (DWORD i = 0; i < DataOut.cbData; ++i)
            ss << std::hex << std::setw(2) << std::setfill(L'0') << (int) DataOut.pbData[i];
        LocalFree(DataOut.pbData);
        return ss.str();
    }
    return L"";
}

std::wstring DecryptString(const std::wstring &hexInput) {
    if (hexInput.empty()) return L"";
    std::vector<BYTE> binary;
    for (size_t i = 0; i < hexInput.length(); i += 2) binary.push_back(
        (BYTE) wcstol(hexInput.substr(i, 2).c_str(), NULL, 16));
    DATA_BLOB DataIn = {(DWORD) binary.size(), binary.data()};
    DATA_BLOB DataOut;
    if (CryptUnprotectData(&DataIn, NULL, NULL, NULL, NULL, 0, &DataOut)) {
        std::wstring result = (wchar_t *) DataOut.pbData;
        LocalFree(DataOut.pbData);
        return result;
    }
    return L"";
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE *) path, (lstrlenW(path) + 1) * sizeof(WCHAR));
        } else RegDeleteValueW(hKey, APP_NAME);
        RegCloseKey(hKey);
    }
}

bool IsAutoStart() {
    HKEY hKey;
    bool exists = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) exists = true;
        RegCloseKey(hKey);
    }
    return exists;
}

void LoadSettings() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    g_UserId = GetPrivateProfileIntW(L"Auth", L"UserId", 0, path);
    g_BgAlpha = GetPrivateProfileIntW(L"Settings", L"BgAlpha", 100, path);
    WCHAR buf[256];
    GetPrivateProfileStringW(L"Auth", L"Username", L"", buf, 256, path);
    g_Username = buf;
    GetPrivateProfileStringW(L"Auth", L"Email", L"", buf, 256, path);
    g_SavedEmail = buf;
    WCHAR pass[1024];
    GetPrivateProfileStringW(L"Auth", L"Pass", L"", pass, 1024, path);
    g_SavedPass = DecryptString(pass);

    WCHAR taiPath[MAX_PATH] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"TaiDbPath", L"", taiPath, MAX_PATH, path);
    g_TaiDbPath = taiPath;
}

void SaveSettings(int uid, const std::wstring &name, const std::wstring &email, const std::wstring &pass, bool savePass) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Auth", L"UserId", std::to_wstring(uid).c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Username", name.c_str(), path);
    WritePrivateProfileStringW(L"Auth", L"Email", email.c_str(), path);
    if (savePass) WritePrivateProfileStringW(L"Auth", L"Pass", EncryptString(pass).c_str(), path);
    else WritePrivateProfileStringW(L"Auth", L"Pass", NULL, path);
}

void SaveAlphaSetting() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Settings", L"BgAlpha", std::to_wstring(g_BgAlpha).c_str(), path);
}

void SaveTaiDbPathSetting() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, SETTINGS_FILE.c_str());
    WritePrivateProfileStringW(L"Settings", L"TaiDbPath", g_TaiDbPath.c_str(), path);
}

HFONT GetMiSansFont(int s) {
    return CreateFontW(S(s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"MiSans");
}