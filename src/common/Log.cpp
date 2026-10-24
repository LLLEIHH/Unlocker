#include "common/Log.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <mutex>
#include <string>

#include "common/StringUtil.h"

namespace ul {
namespace {

std::mutex  g_mutex;
std::wstring g_path;
bool        g_inited = false;

constexpr unsigned long long kMaxLogBytes = 2ull * 1024 * 1024;

const wchar_t* LevelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return L"DBG";
        case LogLevel::Info:  return L"INF";
        case LogLevel::Warn:  return L"WRN";
        case LogLevel::Error: return L"ERR";
    }
    return L"???";
}

void EnsureDir(const std::wstring& path) {
    if (path.empty()) return;
    CreateDirectoryW(path.c_str(), nullptr);
}

}  // namespace

void LogInit(const std::wstring& appName) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_inited) return;
    g_inited = true;

    wchar_t appData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
        GetTempPathW(MAX_PATH, appData);
    }
    std::wstring dir = std::wstring(appData) + L"\\" + appName;
    EnsureDir(std::wstring(appData));
    EnsureDir(dir);
    g_path = dir + L"\\unlocker.log";
}

std::wstring LogFilePath() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_path;
}

void LogWrite(LogLevel level, const std::wstring& msg) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_inited) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    // 轮转
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz{};
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > kMaxLogBytes) {
            std::wstring bak = g_path + L".1";
            DeleteFileW(bak.c_str());
            MoveFileW(g_path.c_str(), bak.c_str());
        }
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             LevelTag(level), msg.c_str());
    fclose(f);
}

void LogAudit(const wchar_t* tag, const std::wstring& rule, const std::wstring& target,
              const std::wstring& canonical, const std::wstring& op, const std::wstring& caller) {
    std::wstring line = std::wstring(L"[") + tag + L"] rule=" + rule +
                        L" target=" + target;
    if (!canonical.empty()) line += L" canonical=" + ToUpper(canonical);
    if (!op.empty())        line += L" op=" + op;
    if (!caller.empty())    line += L" caller=" + caller;
    LogWrite(LogLevel::Info, line);
}

}  // namespace ul
