// UnlockerBundle.exe —— 单文件启动器。
//
// 背景：WinUI 3 的 C++ 版本没有单文件模式（运行时 DLL、resources.pri、*.xbf 都必须落盘），
// 所以这里做一个自解压引导器：把整个 Release 目录压进自己的资源里，首次运行时解到
// %LOCALAPPDATA%\Unlocker\App\<载荷哈希>\ 再启动真正的 Unlocker.exe，之后直接复用缓存。
//
// 设计要点：
//   · 静态 CRT（/MT）：成品不依赖 VC++ 运行库，拷到任何 Win10/11 x64 上都能跑
//   · 哈希做目录名：换了新版本载荷会自动落到新目录，不会和旧文件混在一起
//   · 命名互斥量串行化解压：两个实例同时双击也不会写坏同一份目录
//   · 解压完清理旧版本目录（只保留当前哈希），避免每次升级白占 60MB
//   · 转发命令行参数并等待子进程，退出码原样返回（CLI / --worker 都靠这条）
//   · 每次启动校验已解压文件的 FNV 哈希，对不上就重新解压（自愈 + 防篡改）
//   · 解压目录写显式 DACL（断开父目录继承），只留 当前用户 / SYSTEM / Administrators
#include <windows.h>
#include <aclapi.h>
#include <compressapi.h>

#include <string>
#include <vector>

namespace {

constexpr DWORD kPayloadResourceId = 1;
constexpr wchar_t kPayloadResourceType[] = L"PAYLOAD";
constexpr wchar_t kReadyMarker[] = L"\\ready.tag";

// ---------------------------------------------------------------- 小工具
std::wstring Join(std::wstring const& a, std::wstring const& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

bool FileExists(std::wstring const& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

void EnsureDir(std::wstring const& path) {
    // 逐级创建，CreateDirectoryW 遇到已存在会失败，忽略即可
    std::wstring cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            if (!cur.empty()) CreateDirectoryW(cur.c_str(), nullptr);
        }
        if (i < path.size()) cur.push_back(path[i]);
    }
}

bool DeleteTree(std::wstring const& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return RemoveDirectoryW(dir.c_str()) != 0;

    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring child = Join(dir, fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = DeleteTree(child) && ok;
        } else {
            SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
            ok = DeleteFileW(child.c_str()) != 0 && ok;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    ok = RemoveDirectoryW(dir.c_str()) != 0 && ok;
    return ok;
}

void Fail(std::wstring const& message) {
    MessageBoxW(nullptr, message.c_str(), L"Unlocker",
                MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

// 小端读取
struct Cursor {
    BYTE const* p;
    size_t size;
    size_t off = 0;

    bool Take(void* dst, size_t n) {
        if (off + n > size) return false;
        memcpy(dst, p + off, n);
        off += n;
        return true;
    }
    template <typename T>
    bool Num(T& v) { return Take(&v, sizeof(T)); }
};

// ---------------------------------------------------------------- 载荷
struct Entry {
    std::wstring name;
    uint64_t rawSize = 0;
    uint64_t compSize = 0;
    uint64_t hash = 0;      // FNV-1a 64，启动前用来校验已解压文件
};

struct Payload {
    BYTE const* data = nullptr;
    size_t size = 0;
    uint32_t algo = 0;
    uint64_t tocRawSize = 0;
    std::vector<Entry> entries;
    size_t bodyOffset = 0;      // 各文件压缩块起始位置
};

bool LoadPayloadResource(Payload& out) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(kPayloadResourceId), kPayloadResourceType);
    if (!res) return false;
    HGLOBAL handle = LoadResource(module, res);
    if (!handle) return false;
    out.data = static_cast<BYTE const*>(LockResource(handle));
    out.size = SizeofResource(module, res);
    return out.data != nullptr && out.size > 32;
}

bool Decompress(uint32_t algo, BYTE const* in, size_t inLen, BYTE* out, size_t outLen) {
    COMPRESSOR_HANDLE h = nullptr;
    if (!CreateDecompressor(algo, nullptr, &h)) return false;
    SIZE_T produced = 0;
    const BOOL ok = Decompress(h, const_cast<BYTE*>(in), inLen, out, outLen, &produced);
    CloseDecompressor(h);
    return ok && produced == outLen;
}

bool ParsePayload(Payload& pl) {
    static const char kMagic[8] = { 'U', 'L', 'K', 'P', 'A', 'K', '2', '\0' };
    if (memcmp(pl.data, kMagic, 8) != 0) return false;

    Cursor head{ pl.data, pl.size };
    head.off = 8;
    uint32_t count = 0;
    uint64_t tocCompSize = 0;
    if (!head.Num(pl.algo) || !head.Num(count) || !head.Num(tocCompSize) || !head.Num(pl.tocRawSize)) {
        return false;
    }
    if (tocCompSize > pl.size || pl.tocRawSize > (64u << 20)) return false;

    std::vector<BYTE> toc(static_cast<size_t>(pl.tocRawSize));
    if (!Decompress(pl.algo, pl.data + head.off, static_cast<size_t>(tocCompSize),
                    toc.data(), toc.size())) {
        return false;
    }

    Cursor c{ toc.data(), toc.size() };
    uint32_t n = 0;
    if (!c.Num(n) || n != count) return false;
    pl.entries.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t nameLen = 0;
        if (!c.Num(nameLen) || nameLen > 512) return false;
        std::wstring name(nameLen, L'\0');
        if (!c.Take(name.data(), static_cast<size_t>(nameLen) * sizeof(wchar_t))) return false;
        Entry e;
        e.name = std::move(name);
        if (!c.Num(e.rawSize) || !c.Num(e.compSize) || !c.Num(e.hash)) return false;
        pl.entries.push_back(std::move(e));
    }

    pl.bodyOffset = head.off + static_cast<size_t>(tocCompSize);
    return true;
}

bool ExtractAll(Payload const& pl, std::wstring const& targetDir) {
    size_t offset = pl.bodyOffset;
    for (auto const& e : pl.entries) {
        if (offset + e.compSize > pl.size) return false;

        std::vector<BYTE> buf(static_cast<size_t>(e.rawSize));
        if (!Decompress(pl.algo, pl.data + offset, static_cast<size_t>(e.compSize),
                        buf.data(), buf.size())) {
            return false;
        }
        offset += static_cast<size_t>(e.compSize);

        // 载荷里的相对路径用 '/' 分隔，落盘换成 '\'
        std::wstring rel = e.name;
        for (auto& ch : rel) {
            if (ch == L'/') ch = L'\\';
        }
        const std::wstring full = Join(targetDir, rel);

        const size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos) EnsureDir(full.substr(0, slash));

        HANDLE f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(f, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
        CloseHandle(f);
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------- 目录管理
std::wstring PayloadHash(BYTE const* data, size_t size) {
    // FNV-1a 64：只为「换版本换目录」，不是安全校验
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    wchar_t buf[17]{};
    for (int i = 0; i < 16; ++i) {
        const int nibble = static_cast<int>((h >> (60 - i * 4)) & 0xF);
        buf[i] = nibble < 10 ? static_cast<wchar_t>(L'0' + nibble)
                             : static_cast<wchar_t>(L'a' + nibble - 10);
    }
    return buf;
}

bool HashFile(std::wstring const& path, uint64_t& hash, uint64_t& size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    uint64_t h = 1469598103934665603ull;
    uint64_t total = 0;
    BYTE buf[64 * 1024];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            h ^= buf[i];
            h *= 1099511628211ull;
        }
        total += got;
    }
    CloseHandle(f);
    hash = h;
    size = total;
    return true;
}

// 逐个文件比对载荷里记录的哈希；任何缺失/改动都视为需要重新解压
bool VerifyExtracted(Payload const& pl, std::wstring const& dir) {
    for (auto const& e : pl.entries) {
        std::wstring rel = e.name;
        for (auto& ch : rel) {
            if (ch == L'/') ch = L'\\';
        }
        uint64_t hash = 0;
        uint64_t size = 0;
        if (!HashFile(Join(dir, rel), hash, size)) return false;
        if (size != e.rawSize || hash != e.hash) return false;
    }
    return true;
}

// 给解压目录写一份显式 DACL 并断开继承：父目录（%LOCALAPPDATA%）被人为放宽时，
// 这里不会被顺带放宽。当前用户仍保留完全控制，否则自己升级时删不掉旧版本。
bool HardenDirAcl(std::wstring const& dir) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    const bool got = GetTokenInformation(token, TokenUser, buf.data(), len, &len) != FALSE;
    CloseHandle(token);
    if (!got) return false;
    PSID userSid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;

    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID systemSid = nullptr;
    PSID adminsSid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &systemSid)) {
        return false;
    }
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0,
                                  0, 0, 0, 0, &adminsSid)) {
        FreeSid(systemSid);
        return false;
    }

    PSID sids[3] = { userSid, systemSid, adminsSid };
    EXPLICIT_ACCESS_W entries[3]{};
    for (int i = 0; i < 3; ++i) {
        entries[i].grfAccessPermissions = GENERIC_ALL;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entries[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
    }

    PACL acl = nullptr;
    bool ok = SetEntriesInAclW(3, entries, nullptr, &acl) == ERROR_SUCCESS;
    if (ok) {
        ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
        LocalFree(acl);
    }
    FreeSid(systemSid);
    FreeSid(adminsSid);
    return ok;
}

std::wstring LocalAppData() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return L".";
    return buf;
}

void RemoveOtherVersions(std::wstring const& appRoot, std::wstring const& keep) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(Join(appRoot, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring full = Join(appRoot, fd.cFileName);
        if (_wcsicmp(full.c_str(), keep.c_str()) == 0) continue;
        DeleteTree(full);   // 失败无所谓：下次启动还会再试
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- 启动
std::wstring BuildCommandLine(std::wstring const& exe) {
    std::wstring cmd = L"\"" + exe + L"\"";
    const wchar_t* cl = GetCommandLineW();
    if (*cl == L'"') {
        ++cl;
        while (*cl && *cl != L'"') ++cl;
        if (*cl) ++cl;
    } else {
        while (*cl && *cl != L' ' && *cl != L'\t') ++cl;
    }
    if (*cl) cmd += cl;      // 保留前导空格
    return cmd;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // 从控制台启动时附加到父控制台：引导器是 GUI 子系统、默认没有控制台，
    // 不附加的话内层 exe 的 AttachConsole(ATTACH_PARENT_PROCESS) 会失败，CLI 输出就没了。
    // 子进程会继承这个控制台关联，所以 --status / --list 等参数才有输出。
    AttachConsole(ATTACH_PARENT_PROCESS);

    wchar_t selfPath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    std::wstring selfDir = selfPath;
    const size_t slash = selfDir.find_last_of(L'\\');
    if (slash != std::wstring::npos) selfDir.resize(slash);

    Payload pl;
    if (!LoadPayloadResource(pl) || !ParsePayload(pl)) {
        Fail(L"启动器内的载荷损坏或不完整，请重新下载 Unlocker。\n\n"
             L"The embedded payload is missing or corrupt. Please download Unlocker again.");
        return 2;
    }

    const std::wstring hash = PayloadHash(pl.data, pl.size);
    const std::wstring appRoot = Join(Join(LocalAppData(), L"Unlocker"), L"App");
    const std::wstring targetDir = Join(appRoot, hash);
    const std::wstring exePath = Join(targetDir, L"Unlocker.exe");

    // 每次启动都校验：文件缺失、被改、被截断都会触发重新解压（自愈）
    const bool needExtract = !FileExists(exePath) || !FileExists(targetDir + kReadyMarker) ||
                             !VerifyExtracted(pl, targetDir);

    if (needExtract) {
        // 串行化解压；拿到锁之后再查一次，避免两个实例重复干活
        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Unlocker.Bundle.Extract");
        if (mutex) WaitForSingleObject(mutex, 120000);

        if (!FileExists(exePath) || !FileExists(targetDir + kReadyMarker) ||
            !VerifyExtracted(pl, targetDir)) {
            const std::wstring tempDir = targetDir + L".tmp";
            DeleteTree(tempDir);
            EnsureDir(tempDir);
            if (!HardenDirAcl(tempDir)) {
                // 收紧失败不阻断启动，但要留下痕迹便于排查
                OutputDebugStringW(L"UnlockerBundle: 设置目录 ACL 失败\n");
            }

            if (!ExtractAll(pl, tempDir)) {
                if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
                Fail(L"解压运行所需的文件失败。请检查磁盘空间后重试。\n\n"
                     L"Failed to extract the runtime files. Check free disk space and retry.");
                return 3;
            }

            // 写就绪标记，再原子换名
            HANDLE tag = CreateFileW((tempDir + kReadyMarker).c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (tag != INVALID_HANDLE_VALUE) CloseHandle(tag);

            DeleteTree(targetDir);
            if (!MoveFileExW(tempDir.c_str(), targetDir.c_str(), MOVEFILE_COPY_ALLOWED)) {
                // 换名失败（极少见）：至少保证本次能跑起来
                if (!FileExists(exePath)) {
                    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
                    Fail(L"无法就绪运行目录。\n\nCould not prepare the runtime directory.");
                    return 4;
                }
            }
        }

        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        RemoveOtherVersions(appRoot, targetDir);
    }

    const std::wstring cmd = BuildCommandLine(exePath);
    std::wstring workDir = targetDir;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    // bInheritHandles=TRUE：让内层进程继承控制台句柄（CLI 输出依赖这条）
    if (!CreateProcessW(exePath.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        workDir.c_str(), &si, &pi)) {
        Fail(L"启动 Unlocker 失败（错误码 " + std::to_wstring(GetLastError()) + L"）。\n\n"
             L"Failed to start Unlocker.");
        return 5;
    }

    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
