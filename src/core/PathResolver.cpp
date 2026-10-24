#include "core/PathResolver.h"

#include <winioctl.h>
#include <algorithm>
#include <mutex>
#include <utility>

#include "common/Log.h"
#include "common/StringUtil.h"

namespace ul {
namespace {

struct DeviceMap {
    // 设备前缀 -> 盘符（含尾部反斜杠规范），按长度倒序匹配
    std::vector<std::pair<std::wstring, std::wstring>> entries;  // <\Device\HarddiskVolume3, C:>
};

std::mutex  g_mapMutex;
DeviceMap   g_map;
bool        g_inited = false;

void BuildMapLocked() {
    g_map.entries.clear();
    wchar_t drives[512]{};
    DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    if (n == 0 || n > std::size(drives)) return;
    for (wchar_t* p = drives; *p; p += wcslen(p) + 1) {
        std::wstring root = p;                       // "C:\"
        if (root.size() < 2) continue;
        std::wstring letter = root.substr(0, 2);     // "C:"
        wchar_t target[1024]{};
        if (QueryDosDeviceW(letter.c_str(), target, static_cast<DWORD>(std::size(target)))) {
            g_map.entries.emplace_back(std::wstring(target), letter);
        }
    }
    // 长前缀优先，避免 \Device\HarddiskVolume1 抢走 \Device\HarddiskVolume11
    std::sort(g_map.entries.begin(), g_map.entries.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

}  // namespace

bool PathResolver::Init() {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    if (g_inited) return true;
    g_inited = true;
    BuildMapLocked();
    return !g_map.entries.empty();
}

std::wstring PathResolver::StripPrefix(std::wstring p) {
    for (;;) {
        if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) {
            p = p.substr(4);
            // \\?\UNC\server\share -> \\server\share
            if (p.size() >= 4 && _wcsnicmp(p.c_str(), L"UNC\\", 4) == 0) {
                p = L"\\\\" + p.substr(4);
            }
            continue;
        }
        if (p.size() >= 4 && p.compare(0, 4, L"\\??\\") == 0) {
            p = p.substr(4);
            continue;
        }
        break;
    }
    return p;
}

std::wstring PathResolver::CollapseDots(const std::wstring& p) {
    // 处理 C:\a\..\b 与 C:\a\.\b，就地压栈，不依赖文件是否存在
    if (p.size() < 3) return p;
    const bool unc = IsUncPath(p);

    std::wstring prefix;
    std::wstring rest = p;
    if (!unc && p[1] == L':') {
        prefix = p.substr(0, 3);     // "C:\"
        rest = p.substr(3);
    } else if (unc) {
        // \\server\share 之后的部分才参与折叠
        size_t pos = p.find(L'\\', 2);
        if (pos == std::wstring::npos) return p;
        pos = p.find(L'\\', pos + 1);
        if (pos == std::wstring::npos) return p;
        prefix = p.substr(0, pos + 1);
        rest = p.substr(pos + 1);
    } else if (!p.empty() && p[0] == L'\\') {
        prefix = L"\\";
        rest = p.substr(1);
    }

    std::vector<std::wstring> stack;
    for (auto& seg : Split(rest, L'\\')) {
        if (seg.empty() || seg == L".") continue;
        if (seg == L"..") {
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        stack.push_back(TrimRightDotsSpaces(seg));
    }

    std::wstring out = prefix;
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i) out += L'\\';
        out += stack[i];
    }
    return out;
}

std::wstring PathResolver::GetLongPathBestEffort(const std::wstring& p) {
    wchar_t buf[32768]{};
    DWORD n = GetLongPathNameW(p.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return std::wstring(buf, n);
    return p;
}

std::wstring PathResolver::DeviceToDos(const std::wstring& devicePath) {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    if (!g_inited) { g_inited = true; BuildMapLocked(); }

    for (const auto& [dev, letter] : g_map.entries) {
        if (IStartsWith(devicePath, dev)) {
            std::wstring tail = devicePath.substr(dev.size());
            if (!tail.empty() && tail[0] != L'\\') continue;
            return letter + tail;
        }
    }
    return {};
}

std::wstring PathResolver::DosToDevice(const std::wstring& dosPath) {
    if (dosPath.size() < 2 || dosPath[1] != L':') return {};
    std::wstring letter = dosPath.substr(0, 2);
    wchar_t target[1024]{};
    if (!QueryDosDeviceW(letter.c_str(), target, static_cast<DWORD>(std::size(target)))) return {};
    return std::wstring(target) + dosPath.substr(2);
}

std::wstring PathResolver::Normalize(const std::wstring& rawIn) {
    if (rawIn.empty()) return {};
    Init();

    std::wstring p = rawIn;
    for (auto& c : p) if (c == L'/') c = L'\\';

    p = StripPrefix(p);

    if (p.size() >= 2 && p.compare(0, 2, L"\\\\") != 0) {
        const std::wstring dos = DeviceToDos(p);
        if (!dos.empty()) p = dos;
    }

    // GetFullPathName 负责折叠 . / ..，相对路径也一并基于当前目录解析
    wchar_t buf[32768]{};
    DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf)) p.assign(buf, n);
    else p = CollapseDots(p);

    p = CollapseDots(p);
    p = GetLongPathBestEffort(p);
    p = NormalizeSeparators(p);
    return p;
}

std::wstring PathResolver::Key(const std::wstring& raw) {
    return ToUpper(Normalize(raw));
}

bool PathResolver::PathEqual(const std::wstring& a, const std::wstring& b) {
    return IEquals(Normalize(a), Normalize(b));
}

std::wstring PathResolver::CanonicalFromHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return {};
    wchar_t buf[32768]{};
    // FILE_NAME_NORMALIZED | VOLUME_NAME_DOS：解开 junction / 短名，返回 DOS 卷路径
    DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) {
        n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), 0);
    }
    if (n == 0 || n >= std::size(buf)) return {};
    std::wstring p(buf, n);
    p = StripPrefix(p);
    return NormalizeSeparators(p);
}

std::wstring PathResolver::CanonicalPath(const std::wstring& raw, bool* resolved) {
    if (resolved) *resolved = false;
    if (raw.empty()) return {};

    // 4.6.4：这里绝不能带 FILE_FLAG_OPEN_REPARSE_POINT —— 要的是真身，不是链接本身
    HANDLE h = CreateFileW(raw.c_str(),
                           FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::wstring c = CanonicalFromHandle(h);
        CloseHandle(h);
        if (!c.empty()) {
            if (resolved) *resolved = true;
            return c;
        }
    }

    std::wstring fallback = Normalize(raw);
    std::wstring reparse  = ResolveReparseLoops(fallback);
    if (resolved) *resolved = false;
    return reparse.empty() ? fallback : reparse;
}

namespace {

// REPARSE_DATA_BUFFER 定义在 ntifs.h，SDK 的 winioctl.h 里没有；这里按官方布局自己声明。
struct ReparseBuffer {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    struct {
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        WCHAR  PathBuffer[1];
    } MountPoint;
};

bool ReadReparseTarget(const std::wstring& path, std::wstring& outTarget) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_EA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::vector<BYTE> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE + 16, 0);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(),
                              static_cast<DWORD>(buf.size()), &ret, nullptr);
    CloseHandle(h);
    if (!ok || ret < 8) return false;

    auto* hdr = reinterpret_cast<ReparseBuffer*>(buf.data());
    if (hdr->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT &&
        hdr->ReparseTag != IO_REPARSE_TAG_SYMLINK) {
        return false;
    }

    const USHORT off  = hdr->MountPoint.SubstituteNameOffset;
    const USHORT len  = hdr->MountPoint.SubstituteNameLength;
    if (len == 0) return false;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(
        reinterpret_cast<const BYTE*>(hdr->MountPoint.PathBuffer) + off);
    outTarget.assign(p, len / sizeof(wchar_t));
    outTarget = PathResolver::Normalize(outTarget);
    return !outTarget.empty();
}

}  // namespace

std::wstring PathResolver::ResolveReparseLoops(const std::wstring& path, int maxDepth) {
    if (path.size() < 3) return path;

    std::wstring current = path;
    for (int depth = 0; depth < maxDepth; ++depth) {
        // 逐段推进，遇到重解析点就把前缀替换为它的目标，然后重新开始
        bool replaced = false;
        size_t pos = 0;
        if (!IsUncPath(current) && current.size() > 2 && current[1] == L':') pos = 3;

        for (;;) {
            size_t next = current.find(L'\\', pos);
            if (next == std::wstring::npos) next = current.size();
            std::wstring prefix = current.substr(0, next);
            if (prefix.size() > 2) {
                std::wstring target;
                if (ReadReparseTarget(prefix, target)) {
                    std::wstring tail = current.substr(next);
                    current = target + tail;
                    current = Normalize(current);
                    replaced = true;
                    break;
                }
            }
            if (next >= current.size()) break;
            pos = next + 1;
        }
        if (!replaced) return current;
    }
    return current;
}

std::vector<std::wstring> PathResolver::ComparisonKeys(const std::wstring& path) {
    std::vector<std::wstring> keys;
    auto push = [&keys](const std::wstring& v) {
        if (v.empty()) return;
        std::wstring k = ToUpper(v);
        for (const auto& e : keys) if (e == k) return;
        keys.push_back(k);
    };

    bool resolved = false;
    std::wstring canon = CanonicalPath(path, &resolved);
    push(canon);

    std::wstring norm = Normalize(path);
    push(norm);

    std::wstring dev = DosToDevice(norm);
    if (!dev.empty()) {
        push(NormalizeSeparators(dev));
        push(ToUpper(dev));
    }

    // 8.3 短名：对方可能用短名打开过
    wchar_t shortBuf[32768]{};
    DWORD sn = GetShortPathNameW(norm.c_str(), shortBuf, static_cast<DWORD>(std::size(shortBuf)));
    if (sn > 0 && sn < std::size(shortBuf)) push(std::wstring(shortBuf, sn));

    return keys;
}

bool PathResolver::HasReparsePrefix(const std::wstring& p) {
    return IStartsWith(p, L"\\\\?\\") || IStartsWith(p, L"\\??\\");
}

std::wstring PathResolver::SelfExePath() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::wstring(buf, n);
}

std::wstring PathResolver::SelfDir() {
    std::wstring exe = SelfExePath();
    size_t pos = exe.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : exe.substr(0, pos);
}

}  // namespace ul
