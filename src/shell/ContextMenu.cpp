// 资源管理器右键菜单（F9）。扩展跑在 explorer 进程里（中完整性），
// 所以它只能把路径交给同为中完整性的 UI（通道 A），绝不能直接进入提权通道。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <string>
#include <vector>

#include "common/StringUtil.h"

namespace {

// 必须与 cli/CliMain.cpp 里的 kClsid 一致
const wchar_t* kClsidStr = L"{6E2C4A5B-1B3C-4F62-9C0E-2D8E3A9B7F41}";
const GUID kClsid = { 0x6e2c4a5b, 0x1b3c, 0x4f62, { 0x9c, 0x0e, 0x2d, 0x8e, 0x3a, 0x9b, 0x7f, 0x41 } };
constexpr wchar_t kMenuText[] = L"Unlocker 解锁…";
constexpr int kMenuIdOffset = 0;

long g_refCount = 0;
HINSTANCE g_module = nullptr;

std::wstring ModulePath() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(g_module, buf, static_cast<DWORD>(std::size(buf)));
    return (n == 0) ? std::wstring() : std::wstring(buf, n);
}

std::wstring UnlockerExePath() {
    std::wstring dll = ModulePath();
    const size_t pos = dll.find_last_of(L'\\');
    if (pos == std::wstring::npos) return {};
    return dll.substr(0, pos + 1) + L"Unlocker.exe";
}

std::wstring UiPipeName() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\Unlocker.UI." + std::to_wstring(session);
}

// 通道 A：只传路径，低危唤醒
bool SendToRunningUi(const std::vector<std::wstring>& paths) {
    const std::wstring name = UiPipeName();
    if (!WaitNamedPipeW(name.c_str(), 300)) return false;

    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string payload;
    for (const auto& p : paths) {
        payload += ul::Utf16ToUtf8(p);
        payload.push_back('\n');
    }
    const uint32_t len = static_cast<uint32_t>(payload.size());
    DWORD wrote = 0;
    bool ok = WriteFile(h, &len, sizeof(len), &wrote, nullptr) && wrote == sizeof(len);
    if (ok) ok = WriteFile(h, payload.data(), len, &wrote, nullptr) && wrote == len;
    CloseHandle(h);
    return ok;
}

bool LaunchGui(const std::vector<std::wstring>& paths) {
    const std::wstring exe = UnlockerExePath();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    std::wstring args;
    for (const auto& p : paths) {
        if (!args.empty()) args.push_back(L' ');
        args += L"\"" + p + L"\"";
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(),
                                nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

class CUnlockerMenu : public IShellExtInit, public IContextMenu {
public:
    CUnlockerMenu() { InterlockedIncrement(&g_refCount); }
    ~CUnlockerMenu() { InterlockedDecrement(&g_refCount); }

    // ---------------- IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IShellExtInit)) {
            *ppv = static_cast<IShellExtInit*>(this);
        } else if (IsEqualIID(riid, IID_IContextMenu)) {
            *ppv = static_cast<IContextMenu*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&g_refCount)); }
    STDMETHODIMP_(ULONG) Release() override {
        const long c = InterlockedDecrement(&g_refCount);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    // ---------------- IShellExtInit
    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* dataObj, HKEY) override {
        m_paths.clear();
        if (!dataObj) return E_INVALIDARG;

        FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg{};
        if (FAILED(dataObj->GetData(&fmt, &stg))) return S_OK;   // 没有文件列表就不显示菜单

        HDROP drop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
        if (drop) {
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            // 扩展里绝不做重活：只收集路径（上限 32 个，避免超大选择拖垮 explorer）
            for (UINT i = 0; i < n && i < 32; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring p(len + 1, L'\0');
                DragQueryFileW(drop, i, p.data(), len + 1);
                p.resize(len);
                m_paths.push_back(p);
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        return S_OK;
    }

    // ---------------- IContextMenu
    STDMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT idCmdFirst, UINT, UINT flags) override {
        if (m_paths.empty()) return S_OK;
        if (flags & CMF_DEFAULTONLY) return S_OK;

        InsertMenuW(menu, index, MF_STRING | MF_BYPOSITION,
                    idCmdFirst + kMenuIdOffset, kMenuText);
        // 只返回「用掉了几个命令 ID」的相对偏移 +1
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, kMenuIdOffset + 1);
    }

    STDMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override {
        if (!info) return E_INVALIDARG;
        if (HIWORD(reinterpret_cast<LPARAM>(info->lpVerb)) != 0) return E_INVALIDARG;
        if (LOWORD(reinterpret_cast<LPARAM>(info->lpVerb)) != kMenuIdOffset) return E_INVALIDARG;
        if (m_paths.empty()) return S_OK;

        if (!SendToRunningUi(m_paths)) {
            if (!LaunchGui(m_paths)) {
                MessageBoxW(info->hwnd, L"无法启动 Unlocker.exe，请确认它与 UnlockerShell.dll 在同一目录。",
                            L"Unlocker", MB_ICONERROR | MB_OK);
                return E_FAIL;
            }
        }
        return S_OK;
    }

    STDMETHODIMP GetCommandString(UINT_PTR, UINT flags, UINT*, LPSTR name, UINT) override {
        if (flags == GCS_HELPTEXTW) {
            StringCchCopyW(reinterpret_cast<wchar_t*>(name), 128, L"查看占用进程并解锁");
            return S_OK;
        }
        return E_NOTIMPL;
    }

private:
    std::vector<std::wstring> m_paths;
};

class CClassFactory : public IClassFactory {
public:
    CClassFactory() { InterlockedIncrement(&g_refCount); }
    ~CClassFactory() { InterlockedDecrement(&g_refCount); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&g_refCount)); }
    STDMETHODIMP_(ULONG) Release() override {
        const long c = InterlockedDecrement(&g_refCount);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* obj = new (std::nothrow) CUnlockerMenu();
        if (!obj) return E_OUTOFMEMORY;
        const HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_refCount);
        else InterlockedDecrement(&g_refCount);
        return S_OK;
    }
};

}  // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!IsEqualCLSID(clsid, kClsid)) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) CClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT WINAPI DllCanUnloadNow() {
    return g_refCount == 0 ? S_OK : S_FALSE;
}

HRESULT WINAPI DllRegisterServer() {
    const std::wstring dll = ModulePath();
    if (dll.empty()) return E_FAIL;

    const std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsidStr;
    auto setValue = [](HKEY root, const wchar_t* sub, const wchar_t* name, const std::wstring& value) -> bool {
        HKEY k = nullptr;
        if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
            return false;
        }
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        const bool ok = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) ==
                        ERROR_SUCCESS;
        RegCloseKey(k);
        return ok;
    };

    bool ok = true;
    ok &= setValue(HKEY_CLASSES_ROOT, clsidKey.c_str(), nullptr, L"Unlocker Shell Extension");
    ok &= setValue(HKEY_CLASSES_ROOT, (clsidKey + L"\\InprocServer32").c_str(), nullptr, dll);
    ok &= setValue(HKEY_CLASSES_ROOT, (clsidKey + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Apartment");
    ok &= setValue(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers\\Unlocker", nullptr, kClsidStr);
    ok &= setValue(HKEY_CLASSES_ROOT, L"Directory\\shellex\\ContextMenuHandlers\\Unlocker", nullptr, kClsidStr);
    return ok ? S_OK : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CLASSES_ROOT, (std::wstring(L"CLSID\\") + kClsidStr).c_str());
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers\\Unlocker");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"Directory\\shellex\\ContextMenuHandlers\\Unlocker");
    return S_OK;
}

}  // extern "C"
