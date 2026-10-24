#include "core/PolicyGuard.h"

#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/PathResolver.h"
#include "core/rules/BlacklistTable.h"

#if __has_include("BlacklistCrc.h")
#include "BlacklistCrc.h"
#endif

#ifndef UNLOCKER_RULES_CRC
#define UNLOCKER_RULES_CRC 0u
#define UNLOCKER_RULES_CRC_VALID 0
#endif

namespace ul {
namespace {

// EFI 系统分区 / MSR 保留分区（winioctl.h 没导出这两个 GUID，按官方值声明）
const GUID kPartitionSystemGuid = {
    0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};
const GUID kPartitionMsrGuid = {
    0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE }
};

struct State {
    bool        ready      = false;
    bool        failClosed = false;
    bool        crcOk      = true;
    bool        testBadCrc = false;
    std::wstring windowsDir;
    std::wstring programFiles;
    std::wstring programFilesX86;
    std::wstring selfExeKey;
    std::wstring selfDirKey;
    std::vector<std::wstring> volumeRoots;   // "C:\" 大写
    std::set<std::wstring>    hardDeniedIds; // FILE_ID 集合
    std::vector<std::wstring> expanded;      // 与 kRules 一一对应的展开模式（大写）
};

std::mutex g_mutex;
State      g_state;

std::wstring GetKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

std::wstring UpperSeparators(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    return ToUpper(p);
}

std::wstring ExpandPattern(const std::wstring& pattern) {
    std::wstring p = pattern;
    auto replaceAll = [](std::wstring& s, const std::wstring& from, const std::wstring& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::wstring::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(p, L"<Windows>",         g_state.windowsDir);
    replaceAll(p, L"<ProgramFilesX86>", g_state.programFilesX86);
    replaceAll(p, L"<ProgramFiles>",    g_state.programFiles);
    // <AnyVolumeRoot> 在匹配期逐卷展开，这里保留标记
    return UpperSeparators(p);
}

bool PatternHasValidRoot(const std::wstring& pattern) {
    if (pattern.empty()) return false;
    if (pattern == L"*") return true;
    if (IStartsWith(pattern, L"<WINDOWS>") || IStartsWith(pattern, L"<PROGRAMFILES>") ||
        IStartsWith(pattern, L"<PROGRAMFILESX86>") || IStartsWith(pattern, L"<ANYVOLUMEROOT>"))
        return true;
    if (pattern.size() >= 3 && pattern[1] == L':' && pattern[2] == L'\\') return true;
    if (pattern.size() >= 2 && pattern[0] == L'\\') return true;   // System Volume Information 一类
    return false;
}

bool WildcardMatch(const std::wstring& text, const std::wstring& pat) {
    // 仅支持 * 与 ?，不支持 **（4.6.3）
    size_t ti = 0, pi = 0, star = std::wstring::npos, mark = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && (pat[pi] == L'?' || pat[pi] == text[ti])) {
            ++ti; ++pi;
        } else if (pi < pat.size() && pat[pi] == L'*') {
            star = pi++;
            mark = ti;
        } else if (star != std::wstring::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == L'*') ++pi;
    return pi == pat.size();
}

bool PrefixMatch(const std::wstring& cand, const std::wstring& pat) {
    if (pat.empty()) return false;

    // 以单个反斜杠开头的模式表示「卷根之后的固定片段」，例如 System Volume Information
    if (pat.size() >= 2 && pat[0] == L'\\' && pat[1] != L'\\') {
        size_t pos = 0;
        while ((pos = cand.find(pat, pos)) != std::wstring::npos) {
            if (pos == 0 || cand[pos - 1] == L':') return true;
            ++pos;
        }
        return false;
    }

    std::wstring trimmedPat = pat;
    while (!trimmedPat.empty() && trimmedPat.back() == L'\\') trimmedPat.pop_back();

    // 目录本身也要命中：C:\WINDOWS\SYSTEM32\DRIVERS
    if (cand == trimmedPat) return true;
    if (cand.size() < pat.size()) return false;
    if (cand.compare(0, pat.size(), pat) != 0) return false;
    if (pat.back() == L'\\') return true;
    // 分隔符处切分：\Windows\System 不能匹配 \Windows\System32X
    return cand[pat.size()] == L'\\';
}

bool GlobMatch(const std::wstring& cand, const std::wstring& pat) {
    size_t cp = cand.find_last_of(L'\\');
    size_t pp = pat.find_last_of(L'\\');
    if (cp == std::wstring::npos || pp == std::wstring::npos) return false;
    if (cand.compare(0, cp, pat, 0, pp) != 0) return false;
    return WildcardMatch(cand.substr(cp + 1), pat.substr(pp + 1));
}

bool IsProtectedPartition(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':') return false;
    const std::wstring volume = std::wstring(L"\\\\.\\") + path.substr(0, 2);
    HANDLE h = CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    PARTITION_INFORMATION_EX pie{};
    DWORD ret = 0;
    bool deny = false;
    if (DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &pie, sizeof(pie), &ret, nullptr)) {
        if (pie.PartitionStyle == PARTITION_STYLE_GPT) {
            GUID t = pie.Gpt.PartitionType;
            deny = IsEqualGUID(t, kPartitionSystemGuid) ||
                   IsEqualGUID(t, kPartitionMsrGuid);
        } else if (pie.PartitionStyle == PARTITION_STYLE_MBR) {
            deny = pie.Mbr.PartitionType == 0x27 || pie.Mbr.PartitionType == 0x12;
        }
    }
    CloseHandle(h);
    return deny;
}

struct Accum {
    bool         deny = false;
    RuleId       denyId{};
    Severity     denySeverity = Severity::Crit;
    std::wstring denyReason;

    bool         allowExplicit = false;
    RuleId       allowId{};
    std::wstring allowReason;

    bool         confirm = false;
    RuleId       confirmId{};
    Severity     confirmSeverity = Severity::High;
    std::wstring confirmReason;
};

void MatchCandidate(const std::wstring& cand, Accum& acc) {
    if (cand.empty()) return;

    for (size_t i = 0; i < kRuleCount; ++i) {
        const RuleRow& row = kRules[i];
        const std::wstring& pat = g_state.expanded[i];
        bool hit = false;

        switch (row.type) {
            case MatchType::Exact: {
                if (pat.find(L"<ANYVOLUMEROOT>") != std::wstring::npos) {
                    std::wstring suffix = pat.substr(std::wstring(L"<ANYVOLUMEROOT>").size());
                    for (const auto& root : g_state.volumeRoots) {
                        std::wstring r = UpperSeparators(root);
                        std::wstring tail = ToUpper(suffix);
                        if (!r.empty() && r.back() == L'\\' && !tail.empty() && tail.front() == L'\\') {
                            tail.erase(tail.begin());
                        }
                        if (cand == r + tail) { hit = true; break; }
                    }
                } else {
                    hit = (cand == pat);
                }
                break;
            }
            case MatchType::Prefix:
                hit = PrefixMatch(cand, pat);
                break;
            case MatchType::Glob:
                hit = GlobMatch(cand, pat);
                break;
            case MatchType::VolumeRoot:
                hit = IsVolumeRoot(cand);
                break;
            case MatchType::Volume:
                hit = IsProtectedPartition(cand);
                break;
            case MatchType::Self:
                hit = (!g_state.selfExeKey.empty() && cand == g_state.selfExeKey) ||
                      (!g_state.selfDirKey.empty() && IStartsWith(cand, g_state.selfDirKey + L"\\"));
                break;
            case MatchType::FileId:
            case MatchType::Count:
                hit = false;
                break;
        }

        if (!hit) continue;

        switch (row.verdict) {
            case Verdict::Deny:
                if (!acc.deny) {
                    acc.deny = true;
                    acc.denyId = row.id;
                    acc.denySeverity = row.severity;
                    acc.denyReason = row.reason;
                }
                break;
            case Verdict::Allow:
                if (!acc.allowExplicit) {
                    acc.allowExplicit = true;
                    acc.allowId = row.id;
                    acc.allowReason = row.reason;
                }
                break;
            case Verdict::Confirm:
                if (!acc.confirm) {
                    acc.confirm = true;
                    acc.confirmId = row.id;
                    acc.confirmSeverity = row.severity;
                    acc.confirmReason = row.reason;
                }
                break;
        }
    }
}

PolicyResult FailClosedResult(const std::wstring& raw, const std::wstring& why) {
    PolicyResult r;
    r.verdict = Verdict::Deny;
    r.ruleId = RuleId::BL_NONE;
    r.severity = Severity::Crit;
    r.target = raw;
    r.canonical = raw;
    r.reason = why;
    return r;
}

bool BuildFileIdTable() {
    g_state.hardDeniedIds.clear();
    for (size_t i = 0; i < kRuleCount; ++i) {
        const RuleRow& row = kRules[i];
        if (row.verdict != Verdict::Deny) continue;
        if (row.type != MatchType::Exact && row.type != MatchType::Glob) continue;

        const std::wstring& pat = g_state.expanded[i];
        if (pat.find(L'*') != std::wstring::npos || pat.find(L'?') != std::wstring::npos) continue;
        if (pat.find(L"<ANYVOLUMEROOT>") != std::wstring::npos) continue;

        std::wstring key;
        if (PolicyGuard::GetFileIdKey(pat, &key)) g_state.hardDeniedIds.insert(key);
    }

    // 自身 exe 的 FileId 也进硬禁集合：硬链接到自身时路径规则失效，只能靠 ID 拦（BL-014 的补充）
    {
        std::wstring selfKey;
        if (PolicyGuard::GetFileIdKey(g_state.selfExeKey, &selfKey)) {
            g_state.hardDeniedIds.insert(selfKey);
        }
    }
    return true;
}

bool VerifyRequiredDenyRules() {
    // 语义自检：核心 Deny 规则必须存在且仍是 Deny（防止有人只留了 BL-016 兜底）
    const RuleId required[] = { RuleId::BL_001, RuleId::BL_002, RuleId::BL_003,
                                RuleId::BL_006, RuleId::BL_010, RuleId::BL_019,
                                RuleId::BL_020 };
    for (RuleId want : required) {
        bool found = false;
        for (size_t i = 0; i < kRuleCount; ++i) {
            if (kRules[i].id == want && kRules[i].verdict == Verdict::Deny) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

uint32_t Crc32(const std::string& data) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    });
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : data) crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

bool PolicyGuard::GetFileIdKeyByHandle(HANDLE h, std::wstring* key) {
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info))) return false;

    wchar_t buf[128];
    swprintf_s(buf, L"%08llX-%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
               static_cast<unsigned long long>(info.VolumeSerialNumber),
               info.FileId.Identifier[0],  info.FileId.Identifier[1],
               info.FileId.Identifier[2],  info.FileId.Identifier[3],
               info.FileId.Identifier[4],  info.FileId.Identifier[5],
               info.FileId.Identifier[6],  info.FileId.Identifier[7],
               info.FileId.Identifier[8],  info.FileId.Identifier[9],
               info.FileId.Identifier[10], info.FileId.Identifier[11],
               info.FileId.Identifier[12], info.FileId.Identifier[13],
               info.FileId.Identifier[14], info.FileId.Identifier[15]);
    if (key) *key = buf;
    return true;
}

bool PolicyGuard::GetFileIdKey(const std::wstring& path, std::wstring* key) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 目录/被独占时用 0 访问权限再试一次（Backup 语义配合 SeBackupPrivilege）
        h = CreateFileW(path.c_str(), 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = GetFileIdKeyByHandle(h, key);
    CloseHandle(h);
    return ok;
}

bool PolicyGuard::Init(std::wstring* err) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_state.ready) return !g_state.failClosed;

    PathResolver::Init();

    wchar_t win[MAX_PATH]{};
    if (GetWindowsDirectoryW(win, MAX_PATH) == 0) {
        if (err) *err = L"无法获取 Windows 目录";
        g_state.failClosed = true;
        return false;
    }
    g_state.windowsDir       = NormalizeSeparators(std::wstring(win));
    g_state.programFiles     = GetKnownFolder(FOLDERID_ProgramFiles);
    g_state.programFilesX86  = GetKnownFolder(FOLDERID_ProgramFilesX86);
    g_state.selfExeKey       = ToUpper(PathResolver::Normalize(PathResolver::SelfExePath()));
    g_state.selfDirKey       = ToUpper(PathResolver::Normalize(PathResolver::SelfDir()));

    wchar_t drives[512]{};
    DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    if (n > 0 && n < std::size(drives)) {
        for (wchar_t* p = drives; *p; p += wcslen(p) + 1) g_state.volumeRoots.emplace_back(p);
    }

    g_state.expanded.clear();
    g_state.expanded.reserve(kRuleCount);
    bool badPattern = false;
    for (size_t i = 0; i < kRuleCount; ++i) {
        std::wstring pat = ExpandPattern(kRules[i].pattern);
        if (!PatternHasValidRoot(pat)) {
            badPattern = true;
            LogE(L"黑名单规则模式非法（必须以占位符或盘符开头）：" + pat);
        }
        g_state.expanded.push_back(pat);
    }

    BuildFileIdTable();

    // CRC：构建期从 .def 生成的期望值，运行期从编译进 .rdata 的表重算
    const std::string serialized = SerializeRulesForCrc();
    const uint32_t crc = Crc32(serialized);
    bool crcOk = true;
#if UNLOCKER_RULES_CRC_VALID
    crcOk = (crc == static_cast<uint32_t>(UNLOCKER_RULES_CRC));
#else
    crcOk = true;   // 未生成期望值时不做 CRC 检查（构建告警已提示）
#endif
    if (g_state.testBadCrc) crcOk = false;
    if (!crcOk) {
        LogE(L"规则表 CRC 自检失败，进入 fail-closed：全部操作按 Deny 处理");
    }

    const bool semanticOk = VerifyRequiredDenyRules();
    if (!semanticOk) LogE(L"规则表语义自检失败：核心 Deny 规则缺失或被降级");

    g_state.crcOk      = crcOk;
    g_state.failClosed = badPattern || !crcOk || !semanticOk;
    g_state.ready      = true;

    if (err && g_state.failClosed) *err = L"规则表自检失败，已降级为保护模式（全部拒绝）";
    LogI(L"PolicyGuard 初始化：规则 " + std::to_wstring(kRuleCount) + L" 条，FILE_ID " +
         std::to_wstring(g_state.hardDeniedIds.size()) + L" 个，CRC=" +
         std::to_wstring(crc) + (g_state.failClosed ? L"（fail-closed）" : L""));
    return !g_state.failClosed;
}

bool PolicyGuard::Ready()      { return g_state.ready; }
bool PolicyGuard::FailClosed() { return g_state.failClosed; }

void PolicyGuard::TestForceCrcFailure(bool on) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_state.testBadCrc = on;
    g_state.ready      = false;   // 允许重新走一遍自检
    g_state.failClosed = false;
}

std::string PolicyGuard::SerializeRulesForCrc() {
    std::string out;
    for (size_t i = 0; i < kRuleCount; ++i) {
        const RuleRow& row = kRules[i];
        out += std::to_string(static_cast<int>(row.id));
        out += '|';
        out += std::to_string(static_cast<int>(row.type));
        out += '|';
        out += Utf16ToUtf8(row.pattern);
        out += '|';
        out += std::to_string(static_cast<int>(row.verdict));
        out += '|';
        out += std::to_string(static_cast<int>(row.severity));
        out += '|';
        out += Utf16ToUtf8(row.reason);
        out += '\n';
    }
    return out;
}

PolicyResult PolicyGuard::Evaluate(const std::wstring& rawPath) {
    if (!g_state.ready) {
        std::wstring err;
        Init(&err);
    }
    if (g_state.failClosed) {
        return FailClosedResult(rawPath, L"内置规则表自检失败，为避免误删系统文件，所有操作已临时拒绝。请重新安装本程序。");
    }

    PolicyResult r;
    r.target = rawPath;

    bool resolved = false;
    std::wstring canon = PathResolver::CanonicalPath(rawPath, &resolved);
    if (canon.empty()) {
        canon = PathResolver::Normalize(rawPath);
        resolved = false;
    }
    r.canonical = canon;

    Accum acc;
    MatchCandidate(ToUpper(canon), acc);

    const std::wstring normRaw = PathResolver::Normalize(rawPath);
    if (!IEquals(normRaw, canon)) MatchCandidate(ToUpper(normRaw), acc);

    // FILE_ID 双表：硬链接绕过路径规则（T22）。
    // 路径规则已经命中 Deny 时保留更具体的规则 ID（用户看到 BL-003 比 BL-013 更有用）。
    std::wstring idKey;
    if (!acc.deny && !g_state.hardDeniedIds.empty() && GetFileIdKey(canon, &idKey)) {
        if (g_state.hardDeniedIds.count(idKey)) {
            acc.deny = true;
            acc.denyId = RuleId::BL_013;
            acc.denySeverity = Severity::Crit;
            acc.denyReason = L"该文件与受保护文件是同一个文件（硬链接），路径不同也按受保护处理";
        }
    }

    if (acc.deny) {
        r.verdict = Verdict::Deny;
        r.ruleId = acc.denyId;
        r.severity = acc.denySeverity;
        r.reason = acc.denyReason;
        return r;
    }
    if (acc.allowExplicit) {
        // 显式放行优先于「通用 Confirm」，但永远不能覆盖 Deny（BL-030/031/032 就是这么设计的）
        r.verdict = Verdict::Allow;
        r.ruleId = acc.allowId;
        r.severity = Severity::Info;
        r.reason = acc.allowReason;
        return r;
    }
    if (acc.confirm) {
        r.verdict = Verdict::Confirm;
        r.ruleId = acc.confirmId;
        r.severity = acc.confirmSeverity;
        r.reason = acc.confirmReason;
        return r;
    }

    r.verdict = Verdict::Allow;
    r.ruleId = RuleId::BL_NONE;
    r.severity = Severity::Info;
    r.reason = L"";

    // 4.6.4：拿不到真身就按高危算（从严）
    if (!resolved) {
        r.verdict = Verdict::Confirm;
        r.severity = Severity::High;
        r.reason = L"无法获取该文件的真实路径（可能被独占或拒绝访问），按高危操作处理";
        r.truncated = true;
    }
    return r;
}

PolicyResult PolicyGuard::EvaluateBatch(const std::vector<std::wstring>& paths) {
    PolicyResult worst = Evaluate(paths.empty() ? std::wstring() : paths.front());
    if (paths.empty()) return worst;

    for (size_t i = 1; i < paths.size(); ++i) {
        PolicyResult r = Evaluate(paths[i]);
        if (static_cast<int>(r.verdict) > static_cast<int>(worst.verdict)) worst = r;
    }
    if (paths.size() > 500 && worst.verdict == Verdict::Allow) {
        worst.verdict = Verdict::Confirm;
        worst.ruleId = RuleId::BL_018;
        worst.severity = Severity::High;
        worst.reason = L"本次批量操作对象超过 500 个，请确认不是误拖了整个目录";
    }
    return worst;
}

PolicyResult PolicyGuard::EvaluateProcess(const std::wstring& imageFullPath, DWORD pid) {
    PolicyResult r;
    r.target = imageFullPath;
    r.canonical = imageFullPath;

    if (pid == GetCurrentProcessId()) {
        r.verdict = Verdict::Deny;
        r.ruleId = RuleId::BL_NONE;
        r.severity = Severity::Crit;
        r.reason = L"不能结束本程序自身";
        return r;
    }
    if (!imageFullPath.empty()) {
        PolicyResult pathResult = Evaluate(imageFullPath);
        if (pathResult.Denied()) return pathResult;
        r = pathResult;
    }
    return r;
}

std::wstring PolicyGuard::RuleName(RuleId id) {
    const int idx = static_cast<int>(id);
    if (idx <= 0 || idx > static_cast<int>(std::size(kRuleNames))) return L"BL-000";
    std::wstring name = kRuleNames[idx - 1];
    for (auto& c : name) if (c == L'_') c = L'-';
    return name;
}

const wchar_t* PolicyGuard::VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Allow:   return L"放行";
        case Verdict::Confirm: return L"需确认";
        case Verdict::Deny:    return L"已阻止";
    }
    return L"";
}

std::wstring PolicyGuard::ThreePartMessage(const PolicyResult& r) {
    std::wstring out;
    out += RuleName(r.ruleId);
    out += L" · ";
    out += VerdictName(r.verdict);
    out += L"\n";
    out += r.canonical;
    out += L"\n\n";
    out += r.reason;
    if (r.verdict == Verdict::Deny) {
        out += L"\n此规则为内置保护，无法关闭。";
    }
    return out;
}

}  // namespace ul
