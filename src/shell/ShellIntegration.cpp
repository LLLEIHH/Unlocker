#include "shell/ShellIntegration.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

#include "common/Lang.h"

namespace ul::shell {
namespace {

constexpr const wchar_t* kFileKey  = L"Software\\Classes\\*\\shell\\Unlocker";
constexpr const wchar_t* kDirKey   = L"Software\\Classes\\Directory\\shell\\Unlocker";
constexpr const wchar_t* kBgKey    = L"Software\\Classes\\Directory\\Background\\shell\\Unlocker";

// 旧 COM 扩展（UnlockerShell.dll）留下的键，升级后必须清掉，否则右键会出现两项
constexpr const wchar_t* kLegacyClsid = L"Software\\Classes\\CLSID\\{6E2C4A5B-1B3C-4F62-9C0E-2D8E3A9B7F41}";
constexpr const wchar_t* kLegacyFile  = L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\Unlocker";
constexpr const wchar_t* kLegacyDir   = L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\Unlocker";

constexpr const wchar_t* kAppKey      = L"Software\\Unlocker";
constexpr const wchar_t* kPromptValue = L"ShellMenuAsked";

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return n == 0 ? std::wstring() : std::wstring(buf, n);
}

std::wstring QuotedExe() { return L"\"" + ExePath() + L"\""; }

std::wstring CommandFor(const wchar_t* placeholder) { return QuotedExe() + L" " + placeholder; }

std::wstring IconValue() { return QuotedExe() + L",0"; }

bool SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) ==
           ERROR_SUCCESS;
}

bool WriteEntry(const wchar_t* sub, const std::wstring& text, const std::wstring& cmd, bool multiSelect) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    bool ok = SetString(key, nullptr, text);        // 老版本 shell 读默认值
    ok = SetString(key, L"MUIVerb", text) && ok;    // Win7+ 优先读 MUIVerb
    ok = SetString(key, L"Icon", IconValue()) && ok;
    if (multiSelect) ok = SetString(key, L"MultiSelectModel", L"Player") && ok;
    RegCloseKey(key);
    if (!ok) return false;

    const std::wstring cmdKey = std::wstring(sub) + L"\\command";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, cmdKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    ok = SetString(key, nullptr, cmd);
    RegCloseKey(key);
    return ok;
}

bool ReadCommand(const wchar_t* sub, std::wstring* out) {
    const std::wstring cmdKey = std::wstring(sub) + L"\\command";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, cmdKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;

    wchar_t buf[2048]{};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;
    *out = buf;
    return true;
}

bool KeyPresent(const wchar_t* sub) {
    HKEY key = nullptr;
    const bool ok = RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &key) == ERROR_SUCCESS;
    if (ok) RegCloseKey(key);
    return ok;
}

void DeleteTree(HKEY root, const wchar_t* sub) {
    RegDeleteTreeW(root, sub);   // 不存在时返回 ERROR_FILE_NOT_FOUND，忽略即可
}

}  // namespace

std::wstring MenuText() { return T(Str::ShellMenuVerb); }

bool KeyExists() {
    return KeyPresent(kFileKey) || KeyPresent(kDirKey) || KeyPresent(kBgKey);
}

bool IsInstalled() {
    std::wstring file, dir, bg;
    if (!ReadCommand(kFileKey, &file) || !ReadCommand(kDirKey, &dir) || !ReadCommand(kBgKey, &bg)) {
        return false;
    }
    return file == CommandFor(L"\"%1\"") && dir == CommandFor(L"\"%1\"") &&
           bg == CommandFor(L"\"%V\"");
}

bool Install() {
    const std::wstring text = MenuText();
    if (text.empty()) return false;

    bool ok = true;
    ok = WriteEntry(kFileKey, text, CommandFor(L"\"%1\""), true) && ok;
    ok = WriteEntry(kDirKey, text, CommandFor(L"\"%1\""), true) && ok;
    ok = WriteEntry(kBgKey, text, CommandFor(L"\"%V\""), false) && ok;
    if (ok) NotifyShell();
    return ok;
}

bool Uninstall() {
    DeleteTree(HKEY_CURRENT_USER, kFileKey);
    DeleteTree(HKEY_CURRENT_USER, kDirKey);
    DeleteTree(HKEY_CURRENT_USER, kBgKey);

    // 旧 COM 扩展：HKCU 直接删；HKLM 只有在提权时才删得掉，失败也不影响结果
    DeleteTree(HKEY_CURRENT_USER, kLegacyClsid);
    DeleteTree(HKEY_CURRENT_USER, kLegacyFile);
    DeleteTree(HKEY_CURRENT_USER, kLegacyDir);
    DeleteTree(HKEY_LOCAL_MACHINE, kLegacyClsid);
    DeleteTree(HKEY_LOCAL_MACHINE, kLegacyFile);
    DeleteTree(HKEY_LOCAL_MACHINE, kLegacyDir);

    NotifyShell();
    return true;
}

void NotifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool PromptAnswered() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;

    DWORD value = 0;
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, kPromptValue, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_DWORD && value != 0;
}

void MarkPromptAnswered() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const DWORD one = 1;
    RegSetValueExW(key, kPromptValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
}

}  // namespace ul::shell
