#include "ui/FilePicker.h"

#include <shobjidl.h>
#include <shlwapi.h>

#include "common/StringUtil.h"
#include "core/PathResolver.h"

namespace ul {
namespace {

constexpr wchar_t kLastDirKey[] = L"Software\\Unlocker";
constexpr wchar_t kLastDirValue[] = L"LastDir";

void SaveLastDir(const std::wstring& path) {
    if (path.empty()) return;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kLastDirKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(k, kLastDirValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
                       static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

std::wstring LoadLastDir() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD bytes = sizeof(buf);
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLastDirKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, kLastDirValue, nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &bytes) ==
            ERROR_SUCCESS) {
            RegCloseKey(k);
            return buf;
        }
        RegCloseKey(k);
    }
    return {};
}

void RememberFromResults(IFileOpenDialog* dlg) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            std::wstring path = p;
            CoTaskMemFree(p);
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
                (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
                SaveLastDir(path);
            } else {
                size_t pos = path.find_last_of(L'\\');
                if (pos != std::wstring::npos) SaveLastDir(path.substr(0, pos));
            }
        }
        item->Release();
    }
}

IShellItem* ItemFromPath(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    return item;
}

void SetStartFolder(IFileOpenDialog* dlg) {
    const std::wstring last = LoadLastDir();
    if (last.empty()) return;
    if (IShellItem* item = ItemFromPath(last)) {
        dlg->SetFolder(item);
        item->Release();
    }
}

std::vector<std::wstring> CollectResults(IFileOpenDialog* dlg) {
    std::vector<std::wstring> out;
    IShellItemArray* items = nullptr;
    if (FAILED(dlg->GetResults(&items)) || !items) return out;

    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        IShellItem* it = nullptr;
        PWSTR p = nullptr;
        // 虚拟位置（此电脑、库）上 SIGDN_FILESYSPATH 会失败，跳过而不是当错误弹窗
        if (SUCCEEDED(items->GetItemAt(i, &it)) && it &&
            SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            out.emplace_back(p);
            CoTaskMemFree(p);
        }
        if (it) it->Release();
    }
    items->Release();
    RememberFromResults(dlg);
    return out;
}

}  // namespace

std::vector<std::wstring> PickFiles(HWND owner) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) || !dlg) {
        return out;
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    COMDLG_FILTERSPEC filters[] = {
        { L"所有文件", L"*.*" },
        { L"驱动与模块 (*.sys;*.dll)", L"*.sys;*.dll" },
    };
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetTitle(L"选择要解锁的文件");
    SetStartFolder(dlg);

    if (SUCCEEDED(dlg->Show(owner))) out = CollectResults(dlg);
    dlg->Release();
    return out;
}

std::vector<std::wstring> PickFolders(HWND owner) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) || !dlg) {
        return out;
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"选择要解锁的文件夹");
    SetStartFolder(dlg);

    if (SUCCEEDED(dlg->Show(owner))) out = CollectResults(dlg);
    dlg->Release();
    return out;
}

std::wstring PickNewName(HWND owner, const std::wstring& currentPath) {
    std::wstring result;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) || !dlg) {
        return result;
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN);
    dlg->SetTitle(L"输入新的文件名");

    const std::wstring norm = PathResolver::Normalize(currentPath);
    size_t slash = norm.find_last_of(L'\\');
    const std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : norm.substr(0, slash);
    if (!dir.empty()) {
        if (IShellItem* item = ItemFromPath(dir)) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    dlg->SetFileName(slash == std::wstring::npos ? norm.c_str() : norm.c_str() + slash + 1);

    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        PWSTR p = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                result = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

}  // namespace ul
