#include "core/HandleScanner.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/Log.h"
#include "common/NtApi.h"
#include "common/StringUtil.h"
#include "core/PathResolver.h"

namespace ul {
namespace {

ULONG     g_fileTypeIndex = 0;
bool      g_ready         = false;
std::mutex g_initMutex;

using HandleEntry = SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_;

bool EnumHandles(std::vector<HandleEntry>& out) {
    auto fn = nt::QuerySystemInformation();
    if (!fn) return false;

    ULONG size = 1u << 20;
    std::vector<BYTE> buf;
    for (int attempt = 0; attempt < 8; ++attempt) {
        buf.resize(size);
        ULONG need = 0;
        NTSTATUS st = fn(kSystemExtendedHandleInformation, buf.data(), size, &need);
        if (st == STATUS_INFO_LENGTH_MISMATCH) {
            size = need + 64 * 1024;
            continue;
        }
        if (!NT_SUCCESS(st)) return false;

        auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX_*>(buf.data());
        const size_t count = static_cast<size_t>(info->NumberOfHandles);
        out.assign(info->Handles, info->Handles + count);
        return true;
    }
    return false;
}

DWORD ParentPidOf(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD parent = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) { parent = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return parent;
}

// ---------------------------------------------------------------- 取对象名（带超时）
// NtQueryObject(ObjectNameInformation) 对某些对象会永久阻塞：必须独立线程 + 超时。
// 这里用「一条常驻线程 + 请求事件」而不是每次 CreateThread —— 25k 个句柄的条件下，
// 每句柄建线程本身就要几秒，比扫描还慢。
class NameQuery {
public:
    NameQuery() { Start(); }
    ~NameQuery() { Stop(); }
    NameQuery(const NameQuery&)            = delete;
    NameQuery& operator=(const NameQuery&) = delete;

    bool Query(HANDLE h, std::wstring* out) {
        if (leaks_ >= 20 || !thread_) return false;
        cur_ = h;
        ok_  = false;
        name_.clear();
        ResetEvent(done_);
        SetEvent(req_);

        if (WaitForSingleObject(done_, 100) == WAIT_TIMEOUT) {
            // 卡在不可中断的内核等待时 TerminateThread 不会生效，线程会永久挂着（泄漏栈）
            TerminateThread(thread_, 0);
            CloseHandle(thread_);
            thread_ = nullptr;
            ++leaks_;
            if (leaks_ >= 20) {
                LogE(L"名称查询线程连续超时 20 次，放弃后续句柄名解析（建议重启工具）");
            } else {
                Start();
            }
            return false;
        }
        if (ok_ && out) *out = name_;
        return ok_;
    }

private:
    void Start() {
        if (thread_) return;
        req_  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        done_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!req_ || !done_) { Stop(); return; }
        thread_ = CreateThread(nullptr, 0, &NameQuery::Thunk, this, 0, nullptr);
        if (!thread_) Stop();
    }

    void Stop() {
        if (thread_) {
            quit_ = true;
            SetEvent(req_);
            WaitForSingleObject(thread_, 200);
            TerminateThread(thread_, 0);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (req_)  { CloseHandle(req_);  req_  = nullptr; }
        if (done_) { CloseHandle(done_); done_ = nullptr; }
    }

    static DWORD WINAPI Thunk(LPVOID p) {
        static_cast<NameQuery*>(p)->Loop();
        return 0;
    }

    void Loop() {
        auto queryObject = nt::QueryObject();
        std::vector<BYTE> buf(0x8000);
        for (;;) {
            if (WaitForSingleObject(req_, INFINITE) != WAIT_OBJECT_0) return;
            if (quit_) return;

            ULONG ret = 0;
            NTSTATUS st = queryObject(cur_, kObjectNameInformation, buf.data(),
                                      static_cast<ULONG>(buf.size()), &ret);
            if (NT_SUCCESS(st)) {
                auto* us = reinterpret_cast<PUNICODE_STRING>(buf.data());
                if (us->Buffer && us->Length) {
                    name_.assign(us->Buffer, us->Length / sizeof(wchar_t));
                    ok_ = true;
                }
            }
            SetEvent(done_);
        }
    }

    HANDLE       req_    = nullptr;
    HANDLE       done_   = nullptr;
    HANDLE       thread_ = nullptr;
    HANDLE       cur_    = nullptr;
    bool         ok_     = false;
    bool         quit_   = false;
    std::wstring name_;
    int          leaks_  = 0;
};

// ---------------------------------------------------------------- 进程身份补全
void FillVersionInfo(const std::wstring& path, ProcessInfo& pi) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return;

    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return;

    struct LangCp { WORD lang; WORD cp; };
    std::vector<LangCp> langs;
    LangCp* trans = nullptr;
    UINT transLen = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&trans), &transLen) &&
        transLen >= sizeof(LangCp)) {
        const size_t n = transLen / sizeof(LangCp);
        for (size_t i = 0; i < n && langs.size() < 16; ++i) langs.push_back(trans[i]);
    }
    // 语言块必须回退：中文系统上大量程序只有 080404B0
    langs.push_back({0x0804, 0x04B0});
    langs.push_back({0x0409, 0x04B0});
    langs.push_back({0x0409, 0x04E4});
    langs.push_back({0x0000, 0x04B0});

    for (const auto& lc : langs) {
        wchar_t key[128];
        if (pi.description.empty()) {
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", lc.lang, lc.cp);
            wchar_t* v = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), key, reinterpret_cast<LPVOID*>(&v), &len) && v && len) {
                pi.description.assign(v, wcsnlen(v, len));
            }
        }
        if (pi.company.empty()) {
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\CompanyName", lc.lang, lc.cp);
            wchar_t* v = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), key, reinterpret_cast<LPVOID*>(&v), &len) && v && len) {
                pi.company.assign(v, wcsnlen(v, len));
            }
        }
        if (!pi.description.empty() && !pi.company.empty()) break;
    }
}

ProcessInfo QueryProcessInfo(DWORD pid) {
    ProcessInfo pi;
    pi.pid = pid;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    }
    if (!h) {
        pi.protectedProc = true;
        pi.name = L"[无法读取进程信息]";
        return pi;
    }

    wchar_t buf[32768]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        pi.fullPath.assign(buf, len);
    } else {
        HMODULE mod = nullptr;
        DWORD need = 0;
        if (EnumProcessModules(h, &mod, sizeof(mod), &need) && mod) {
            wchar_t mpath[MAX_PATH * 4]{};
            if (GetModuleFileNameExW(h, mod, mpath, static_cast<DWORD>(std::size(mpath)))) {
                pi.fullPath = mpath;
            }
        }
    }

    if (!pi.fullPath.empty()) {
        size_t pos = pi.fullPath.find_last_of(L'\\');
        pi.name = (pos == std::wstring::npos) ? pi.fullPath : pi.fullPath.substr(pos + 1);
        FillVersionInfo(pi.fullPath, pi);
    } else {
        pi.name = L"[无法读取进程信息]";
    }

    HANDLE tok = nullptr;
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e{};
        DWORD n = 0;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n)) pi.elevated = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    CloseHandle(h);
    return pi;
}

bool ProcessMapsPath(DWORD pid, const std::wstring& canonical) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (me.szExePath[0] &&
                IEquals(PathResolver::Normalize(me.szExePath), canonical)) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

std::vector<DWORD> AllPids() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { pids.push_back(pe.th32ProcessID); } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

struct HitRecord {
    DWORD pid;
    HANDLE handleValue;
    ULONG grantedAccess;
};

}  // namespace

int HandleScanner::FileTypeIndex() { return static_cast<int>(g_fileTypeIndex); }
bool HandleScanner::Ready() { return g_ready; }

bool HandleScanner::Init(std::wstring* err) {
    std::lock_guard<std::mutex> lk(g_initMutex);
    if (g_ready) return true;
    if (!nt::Init()) {
        if (err) *err = L"无法解析 ntdll 导出（NtQuerySystemInformation 不可用）";
        return false;
    }

    // 对象类型索引每次开机都变，不能硬编码：打开自身 exe，在句柄表里找到它，取 ObjectTypeIndex
    const std::wstring self = PathResolver::SelfExePath();
    HANDLE hSelf = CreateFileW(self.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hSelf == INVALID_HANDLE_VALUE) {
        hSelf = CreateFileW(self.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (hSelf != INVALID_HANDLE_VALUE) {
        std::vector<HandleEntry> handles;
        if (EnumHandles(handles)) {
            const DWORD selfPid = GetCurrentProcessId();
            for (const auto& e : handles) {
                if (static_cast<DWORD>(e.UniqueProcessId) == selfPid && e.HandleValue == hSelf) {
                    g_fileTypeIndex = e.ObjectTypeIndex;
                    break;
                }
            }
        }
        CloseHandle(hSelf);
    }

    if (g_fileTypeIndex == 0) {
        if (err) *err = L"无法探测文件对象类型索引";
        return false;
    }
    g_ready = true;
    LogI(L"HandleScanner 就绪，文件对象类型索引=" + std::to_wstring(g_fileTypeIndex));
    return true;
}

bool HandleScanner::Probe(const std::wstring& path, OccupancyState* state, DWORD* win32Error) {
    if (state) *state = OccupancyState::Error;
    if (win32Error) *win32Error = 0;

    // 这里要删的是链接本身，所以必须带 FILE_FLAG_OPEN_REPARSE_POINT（与 4.6.4 归一化相反）
    HANDLE h = CreateFileW(path.c_str(), DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        if (state) *state = OccupancyState::Opened;
        return true;
    }

    const DWORD e = GetLastError();
    if (win32Error) *win32Error = e;

    switch (e) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            if (state) *state = OccupancyState::Locked;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            if (state) *state = OccupancyState::AccessDenied;
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            if (state) *state = OccupancyState::NotFound;
            break;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_PARAMETER:
        default:
            // default 绝对不能返回 Free：未知失败一律从严，让上层如实报错
            if (state) *state = OccupancyState::Error;
            break;
    }
    return false;
}

std::vector<HandleOwner> HandleScanner::FindOwners(const std::wstring& path,
                                                   std::vector<DWORD>* skippedPids,
                                                   std::atomic<bool>* cancel) {
    std::vector<HandleOwner> owners;
    if (!g_ready) {
        std::wstring e;
        Init(&e);
    }
    if (!g_ready) return owners;

    std::vector<HandleEntry> handles;
    if (!EnumHandles(handles)) return owners;

    // 比对键：真身 / 归一化 / 设备形式 / 短名
    std::unordered_set<std::wstring> keySet;
    for (const auto& k : PathResolver::ComparisonKeys(path)) keySet.insert(k);

    std::unordered_map<DWORD, std::vector<HandleEntry>> byPid;
    for (const auto& e : handles) {
        if (e.ObjectTypeIndex != g_fileTypeIndex) continue;
        byPid[static_cast<DWORD>(e.UniqueProcessId)].push_back(e);
    }

    std::set<DWORD> skip;
    skip.insert(GetCurrentProcessId());
    const DWORD parent = ParentPidOf(GetCurrentProcessId());
    if (parent) skip.insert(parent);   // worker 侧排掉 UI；UI 侧排掉自己

    std::vector<std::pair<DWORD, std::vector<HandleEntry>*>> work;
    work.reserve(byPid.size());
    for (auto& kv : byPid) {
        if (skip.count(kv.first)) continue;
        work.emplace_back(kv.first, &kv.second);
    }

    const unsigned hw = std::max(1u, std::min<unsigned>(8u, std::thread::hardware_concurrency()));
    std::vector<std::vector<HitRecord>> hits(hw);
    std::vector<std::vector<DWORD>>       skipped(hw);
    std::atomic<size_t> next{0};

    auto worker = [&](unsigned slot) {
        NameQuery nq;
        for (;;) {
            if (cancel && cancel->load()) return;
            const size_t i = next.fetch_add(1);
            if (i >= work.size()) return;

            const DWORD pid = work[i].first;
            const auto& entries = *work[i].second;

            HANDLE h = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) {
                // 打开失败 = 整个进程的句柄都没解析，只能记 skipped，不能当作「没人占用」
                skipped[slot].push_back(pid);
                continue;
            }

            for (const auto& e : entries) {
                HANDLE dup = nullptr;
                if (!DuplicateHandle(h, e.HandleValue, GetCurrentProcess(), &dup,
                                     0, FALSE, 0)) {
                    continue;
                }
                std::wstring name;
                bool matched = false;
                if (nq.Query(dup, &name) && !name.empty()) {
                    const std::wstring upper = ToUpper(name);
                    if (keySet.count(upper)) {
                        matched = true;
                    } else if (const std::wstring dos = PathResolver::DeviceToDos(name); !dos.empty()) {
                        matched = keySet.count(ToUpper(dos)) > 0;
                    }
                }
                CloseHandle(dup);
                if (matched) hits[slot].push_back({pid, e.HandleValue, e.GrantedAccess});
            }
            CloseHandle(h);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(hw);
        for (unsigned i = 0; i < hw; ++i) pool.emplace_back(worker, i);
        for (auto& t : pool) t.join();
    }

    std::map<DWORD, ProcessInfo> infoCache;
    for (auto& vec : hits) {
        for (const auto& hit : vec) {
            auto it = infoCache.find(hit.pid);
            if (it == infoCache.end()) {
                ProcessInfo pi = QueryProcessInfo(hit.pid);
                it = infoCache.emplace(hit.pid, std::move(pi)).first;
            }
            HandleOwner o;
            o.proc          = it->second;
            o.handleValue   = hit.handleValue;
            o.grantedAccess = hit.grantedAccess;
            owners.push_back(o);
        }
    }

    // 同一进程对同一文件的多个句柄聚合计数（UI 显示 ×3，不铺 3 行）
    std::map<DWORD, int> counts;
    for (const auto& o : owners) ++counts[o.proc.pid];
    for (auto& o : owners) o.proc.handleCount = counts[o.proc.pid];

    // 按进程排序（句柄多的在前），UI 直接渲染
    std::stable_sort(owners.begin(), owners.end(), [&counts](const HandleOwner& a, const HandleOwner& b) {
        if (a.proc.pid != b.proc.pid) return counts[a.proc.pid] > counts[b.proc.pid];
        return false;
    });

    if (skippedPids) {
        for (auto& s : skipped) skippedPids->insert(skippedPids->end(), s.begin(), s.end());
    }
    return owners;
}

bool HandleScanner::IsImageMappedBy(const std::wstring& canonical, const std::vector<DWORD>& pids) {
    for (DWORD pid : pids) {
        if (ProcessMapsPath(pid, canonical)) return true;
    }
    return false;
}

FileReport HandleScanner::Scan(const std::wstring& path, const ScanOptions& opts,
                               const ProgressFn& progress, std::atomic<bool>* cancel) {
    FileReport rep;
    rep.path = path;

    if (!g_ready) {
        std::wstring e;
        if (!Init(&e)) {
            rep.state = OccupancyState::Error;
            rep.error = ERROR_NOT_SUPPORTED;
            LogE(L"HandleScanner 初始化失败：" + e);
            return rep;
        }
    }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        rep.directory = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        rep.size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (rep.directory) rep.size = 0;
    }

    OccupancyState st = OccupancyState::Error;
    DWORD err = 0;
    Probe(path, &st, &err);
    rep.state = st;
    rep.error = err;

    bool resolved = false;
    rep.canonical = PathResolver::CanonicalPath(path, &resolved);
    if (rep.canonical.empty()) rep.canonical = PathResolver::Normalize(path);

    if (st == OccupancyState::NotFound || st == OccupancyState::Error) return rep;
    if (cancel && cancel->load()) return rep;

    if (progress) progress(L"枚举系统句柄", 0, 0);
    rep.owners = FindOwners(path, &rep.skippedPids, cancel);
    rep.totalFileHandles = static_cast<int>(rep.owners.size());

    // 去重 skippedPids
    std::sort(rep.skippedPids.begin(), rep.skippedPids.end());
    rep.skippedPids.erase(std::unique(rep.skippedPids.begin(), rep.skippedPids.end()),
                          rep.skippedPids.end());

    // 映像映射：去查模块列表（GrantedAccess 里不存在 SECTION_MAP_EXECUTE，不能拿它判断）
    bool imageMapped = false;
    if (opts.detectImageMapping) {
        std::vector<DWORD> candidates;
        for (const auto& o : rep.owners) candidates.push_back(o.proc.pid);
        if (candidates.empty() && st == OccupancyState::Locked) {
            // 句柄枚举不到但删不掉：可能只有映像映射（section 句柄不等于文件句柄）
            candidates = AllPids();
        }
        if (!candidates.empty()) {
            std::set<DWORD> mapped;
            for (DWORD pid : candidates) {
                if (ProcessMapsPath(pid, rep.canonical)) mapped.insert(pid);
            }
            if (!mapped.empty()) {
                imageMapped = true;
                for (auto& o : rep.owners) {
                    if (mapped.count(o.proc.pid)) o.isImageMapping = true;
                }
            }
        }
    }

    switch (st) {
        case OccupancyState::Opened:
            rep.state = rep.owners.empty() ? OccupancyState::Free
                                           : (imageMapped ? OccupancyState::ImageMapped
                                                          : OccupancyState::Opened);
            break;
        case OccupancyState::Locked:
            if (imageMapped)                             rep.state = OccupancyState::ImageMapped;
            else if (rep.owners.empty())                 rep.state = OccupancyState::LockedByKernel;
            else                                         rep.state = OccupancyState::Locked;
            break;
        case OccupancyState::AccessDenied:
            rep.state = OccupancyState::AccessDenied;
            break;
        default:
            break;
    }

    if (!opts.elevated) {
        if (st == OccupancyState::AccessDenied) rep.needElevation = true;
        if (!rep.skippedPids.empty() &&
            (rep.state == OccupancyState::Locked || rep.state == OccupancyState::LockedByKernel)) {
            rep.needElevation = true;
        }
    }

    if (progress) progress(L"完成", 1, 1);
    return rep;
}

}  // namespace ul
