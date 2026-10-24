#include "core/FileOps.h"

#include <algorithm>
#include <map>
#include <set>

#include "common/ErrorMap.h"
#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/HandleScanner.h"
#include "core/LockBreaker.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "core/RebootScheduler.h"

namespace ul {
namespace {

constexpr size_t kMaxPlanItems = 200000;

struct PlanItem {
    std::wstring path;
    bool         dir     = false;
    bool         deny    = false;
    bool         confirm = false;
    std::wstring rule;
    std::wstring reason;
};

std::wstring BaseName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

void BuildPlan(const std::wstring& path, std::vector<PlanItem>& plan, size_t& count) {
    if (count >= kMaxPlanItems) return;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) != FALSE;
    const bool isDir = exists && (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    const bool reparse = exists && (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);

    PolicyResult pol = PolicyGuard::Evaluate(path);

    PlanItem item;
    item.path = path;
    item.dir  = isDir;
    if (pol.Denied()) {
        item.deny = true;
        item.rule = PolicyGuard::RuleName(pol.ruleId);
        item.reason = pol.reason;
        plan.push_back(item);
        LogAudit(L"DENY", item.rule, path, pol.canonical, L"DeletePlan", L"core");
        return;   // 整棵子树都不动
    }
    if (pol.NeedsConfirm()) {
        item.confirm = true;
        item.rule = PolicyGuard::RuleName(pol.ruleId);
        item.reason = pol.reason;
    }

    if (isDir && !reparse) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (count >= kMaxPlanItems) break;
                BuildPlan(JoinPath(path, fd.cFileName), plan, count);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }

    plan.push_back(item);   // 目录本身排在子项之后
    ++count;
}

struct DeleteOutcome {
    bool         ok = false;
    int          level = -1;
    bool         reboot = false;
    DWORD        code = 0;
    std::wstring message;
    std::vector<std::wstring> owners;
};

DeleteOutcome DeleteOne(const std::wstring& path, bool isDir, const ApplyOptions& opts) {
    DeleteOutcome out;
    DWORD lastErr = 0;

    auto tryRemove = [&](bool clearReadonly) -> bool {
        if (clearReadonly) LockBreaker::ClearReadOnly(path);
        BOOL ok = isDir ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
        if (!ok) lastErr = GetLastError();
        return ok != FALSE;
    };

    // Level 0：正常删除
    if (tryRemove(false)) { out.ok = true; out.level = 0; return out; }
    if (lastErr == ERROR_ACCESS_DENIED) {
        if (tryRemove(true)) { out.ok = true; out.level = 0; return out; }
    }
    out.code = lastErr;

    // Level 1：POSIX 语义删除（只读属性 + 假性占用）
    if (!isDir) {
        DWORD posixErr = 0;
        if (LockBreaker::PosixDelete(path, &posixErr)) { out.ok = true; out.level = 1; return out; }
        if (posixErr) out.code = posixErr;
    }

    // Level 2：关闭远程句柄后重试
    {
        std::vector<DWORD> pids;
        DWORD closeErr = 0;
        int closed = LockBreaker::CloseHandlesForPath(path, 3, &pids, &closeErr);
        if (closed > 0) {
            for (DWORD pid : pids) {
                for (auto& s : LockBreaker::DescribeProcesses({pid})) out.owners.push_back(s);
            }
            if (tryRemove(true)) { out.ok = true; out.level = 2; return out; }
            if (!isDir) {
                DWORD e2 = 0;
                if (LockBreaker::PosixDelete(path, &e2)) { out.ok = true; out.level = 2; return out; }
                if (e2) out.code = e2;
            } else {
                out.code = lastErr;
            }
        } else {
            if (closeErr) out.code = closeErr;
        }
    }

    // Level 4：终止持有进程（强制模式）
    if (opts.force) {
        std::vector<DWORD> skipped;
        auto owners = HandleScanner::FindOwners(path, &skipped, nullptr);
        std::set<DWORD> pids;
        for (const auto& o : owners) pids.insert(o.proc.pid);

        bool killedAny = false;
        for (DWORD pid : pids) {
            std::wstring msg;
            if (LockBreaker::KillProcess(pid, true, &msg)) killedAny = true;
            out.owners.push_back(msg);
        }
        if (killedAny) {
            Sleep(100);
            if (tryRemove(true)) { out.ok = true; out.level = 4; return out; }
            if (!isDir) {
                DWORD e3 = 0;
                if (LockBreaker::PosixDelete(path, &e3)) { out.ok = true; out.level = 4; return out; }
            }
        }
    }

    // Level 5：重启兜底（同样要过黑名单，前面已经过完）
    if (opts.allowReboot) {
        DWORD rebootErr = 0;
        if (RebootScheduler::ScheduleDelete(path, &rebootErr)) {
            out.ok = true;
            out.level = 5;
            out.reboot = true;
            out.message = RebootScheduler::Explain();
            return out;
        }
        if (rebootErr) out.code = rebootErr;
    }

    out.ok = false;
    out.message = ErrorWithCode(out.code);
    if (out.code == ERROR_SHARING_VIOLATION) {
        out.message += L"。文件仍被占用；可尝试强制模式，或排队到重启后删除。";
    } else if (out.code == ERROR_ACCESS_DENIED) {
        out.message += L"。需要管理员权限，或该文件受系统保护。";
    } else if (isDir && out.code == ERROR_DIR_NOT_EMPTY) {
        out.message += L"。目录内还有未能删除的项目。";
    }
    return out;
}

// 统一预检：把 Confirm 项聚合成一次确认请求
bool AskConfirmOnce(const std::vector<PlanItem>& plan, const ConfirmFn& confirm,
                    const ApplyOptions& opts, const wchar_t* opName,
                    std::set<std::wstring>* declinedRules) {
    std::vector<std::wstring> paths;
    std::wstring rule, reason;
    for (const auto& it : plan) {
        if (!it.confirm) continue;
        paths.push_back(it.path);
        if (rule.empty()) rule = it.rule;
        if (reason.empty()) reason = it.reason;
    }
    if (paths.empty()) return true;

    if (!confirm || opts.quiet) return false;   // quiet 下无人可确认，安全中止

    ConfirmRequest req;
    req.paths  = std::move(paths);
    req.rule   = rule;
    req.reason = reason;
    req.batch  = true;
    req.op     = opName;

    const bool yes = confirm(req);
    if (yes) {
        LogAudit(L"CONFIRM-YES", rule, req.paths.front(), L"", opName, opts.caller);
    } else {
        LogAudit(L"CONFIRM-NO", rule, req.paths.front(), L"", opName, opts.caller);
        if (declinedRules) declinedRules->insert(rule);
    }
    return yes;
}

void Tally(BatchResult& batch) {
    batch.okCount = 0;
    batch.failCount = 0;
    batch.skipCount = 0;
    for (const auto& it : batch.items) {
        if (it.skipped) ++batch.skipCount;
        else if (it.ok) ++batch.okCount;
        else ++batch.failCount;
        if (it.rebootRequired) batch.anyReboot = true;
    }
}

BatchResult DoDelete(const std::vector<std::wstring>& paths, const ApplyOptions& opts,
                     const ConfirmFn& confirm) {
    BatchResult batch;

    std::vector<PlanItem> plan;
    size_t count = 0;
    for (const auto& p : paths) BuildPlan(p, plan, count);

    std::set<std::wstring> declined;
    const bool confirmed = AskConfirmOnce(plan, confirm, opts, L"Delete", &declined);
    if (!confirmed) {
        for (const auto& it : plan) {
            if (it.confirm) { batch.needConfirm = true; break; }
        }
    }

    for (const auto& it : plan) {
        ItemResult r;
        r.path = it.path;
        if (it.deny) {
            r.skipped = true;
            r.code = kExitPolicyDenied;
            r.rule = it.rule;
            r.message = it.reason;
            batch.items.push_back(r);
            continue;
        }
        if (it.confirm && !confirmed) {
            r.skipped = true;
            r.code = kExitNeedConfirm;
            r.rule = it.rule;
            r.message = L"需要二次确认，但当前无人可确认（quiet 模式或已取消）";
            batch.items.push_back(r);
            continue;
        }

        DeleteOutcome o = DeleteOne(it.path, it.dir, opts);
        r.ok        = o.ok;
        r.level     = o.level;
        r.code      = o.ok ? 0 : o.code;
        r.rule      = it.rule;
        r.message   = o.ok ? (o.reboot ? o.message : L"已删除") : o.message;
        r.rebootRequired = o.reboot;
        r.owners    = o.owners;
        batch.items.push_back(r);
    }

    Tally(batch);
    return batch;
}

// ---------------------------------------------------------------- 复制
bool CopyOne(const std::wstring& src, const std::wstring& dst, const ApplyOptions& opts,
             std::wstring* msg) {
    auto doCopy = [&]() -> bool {
        return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
    };

    if (!doCopy()) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION) {
            LockBreaker::CloseHandlesForPath(src, 3, nullptr, nullptr);
            if (!doCopy()) {
                if (msg) *msg = L"复制失败：" + ErrorWithCode(GetLastError());
                return false;
            }
        } else if (e == ERROR_ACCESS_DENIED) {
            if (msg) *msg = L"复制失败：" + ErrorWithCode(e);
            return false;
        } else {
            if (msg) *msg = L"复制失败：" + ErrorWithCode(e);
            return false;
        }
    }

    // 保留时间戳与属性
    HANDLE hs = CreateFileW(src.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hs != INVALID_HANDLE_VALUE) {
        FILETIME c, a, w;
        if (GetFileTime(hs, &c, &a, &w)) {
            HANDLE hd = CreateFileW(dst.c_str(), FILE_WRITE_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (hd != INVALID_HANDLE_VALUE) {
                SetFileTime(hd, &c, &a, &w);
                CloseHandle(hd);
            }
        }
        CloseHandle(hs);
    }
    DWORD attr = GetFileAttributesW(src.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(dst.c_str(), attr);
    (void)opts;
    return true;
}

bool CopyTree(const std::wstring& src, const std::wstring& dst, const ApplyOptions& opts,
              std::wstring* msg) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fad)) {
        if (msg) *msg = L"源路径不存在";
        return false;
    }
    const bool isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool reparse = (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    if (!isDir || reparse) return CopyOne(src, dst, opts, msg);

    if (!CreateDirectoryW(dst.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (msg) *msg = L"创建目录失败：" + ErrorWithCode(GetLastError());
        return false;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(JoinPath(src, L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!CopyTree(JoinPath(src, fd.cFileName), JoinPath(dst, fd.cFileName), opts, msg)) {
            FindClose(find);
            return false;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return true;
}

}  // namespace

BatchResult FileOps::Unlock(const std::vector<std::wstring>& paths, const ApplyOptions& opts,
                            const ConfirmFn& confirm) {
    BatchResult batch;
    for (const auto& p : paths) {
        ItemResult r;
        r.path = p;

        PolicyResult pol = PolicyGuard::Evaluate(p);
        if (pol.Denied()) {
            r.code = kExitPolicyDenied;
            r.rule = PolicyGuard::RuleName(pol.ruleId);
            r.message = pol.reason;
            r.skipped = true;
            LogAudit(L"DENY", r.rule, p, pol.canonical, L"Unlock", opts.caller);
            batch.items.push_back(r);
            continue;
        }
        if (pol.NeedsConfirm()) {
            if (!confirm || opts.quiet) {
                r.skipped = true;
                r.code = kExitNeedConfirm;
                r.rule = PolicyGuard::RuleName(pol.ruleId);
                r.message = L"需要二次确认，但当前无人可确认";
                batch.needConfirm = true;
                batch.items.push_back(r);
                continue;
            }
            ConfirmRequest req;
            req.paths.push_back(p);
            req.rule = PolicyGuard::RuleName(pol.ruleId);
            req.reason = pol.reason;
            req.op = L"Unlock";
            if (!confirm(req)) {
                r.skipped = true;
                r.code = kExitNeedConfirm;
                r.message = L"用户取消了确认";
                batch.items.push_back(r);
                continue;
            }
        }

        std::vector<DWORD> pids;
        DWORD err = 0;
        int closed = LockBreaker::CloseHandlesForPath(p, 3, &pids, &err);
        r.closedHandles = closed;
        r.ok = true;
        r.level = 2;
        r.message = closed > 0
                        ? (L"已关闭 " + std::to_wstring(closed) + L" 个句柄")
                        : L"没有找到可关闭的句柄";
        for (auto& s : LockBreaker::DescribeProcesses(pids)) r.owners.push_back(s);
        batch.items.push_back(r);
    }
    Tally(batch);
    return batch;
}

BatchResult FileOps::Delete(const std::vector<std::wstring>& paths, const ApplyOptions& opts,
                            const ConfirmFn& confirm) {
    return DoDelete(paths, opts, confirm);
}

BatchResult FileOps::Rename(const std::wstring& path, const std::wstring& newName,
                            const ApplyOptions& opts, const ConfirmFn& confirm) {
    BatchResult batch;
    ItemResult r;
    r.path = path;

    if (newName.empty() || newName.find(L'\\') != std::wstring::npos) {
        r.code = ERROR_INVALID_NAME;
        r.message = L"新名称不能为空，也不能包含路径分隔符";
        batch.items.push_back(r);
        Tally(batch);
        return batch;
    }

    PolicyResult pol = PolicyGuard::Evaluate(path);
    if (pol.Denied()) {
        r.skipped = true;
        r.code = kExitPolicyDenied;
        r.rule = PolicyGuard::RuleName(pol.ruleId);
        r.message = pol.reason;
        batch.items.push_back(r);
        Tally(batch);
        return batch;
    }
    if (pol.NeedsConfirm() && (!confirm || opts.quiet)) {
        r.skipped = true;
        r.code = kExitNeedConfirm;
        r.ok = false;
        batch.needConfirm = true;
        r.message = L"需要二次确认，但当前无人可确认";
        batch.items.push_back(r);
        Tally(batch);
        return batch;
    }
    if (pol.NeedsConfirm()) {
        ConfirmRequest req;
        req.paths.push_back(path);
        req.rule = PolicyGuard::RuleName(pol.ruleId);
        req.reason = pol.reason;
        req.op = L"Rename";
        if (!confirm(req)) {
            r.skipped = true;
            r.code = kExitNeedConfirm;
            r.message = L"用户取消了确认";
            batch.items.push_back(r);
            Tally(batch);
            return batch;
        }
    }

    const std::wstring norm = PathResolver::Normalize(path);
    const size_t slash = norm.find_last_of(L'\\');
    const std::wstring dest = (slash == std::wstring::npos ? std::wstring() : norm.substr(0, slash + 1)) + newName;

    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (MoveFileExW(norm.c_str(), dest.c_str(), 0)) {
            r.ok = true;
            r.level = 0;
            r.message = L"已重命名为 " + newName;
            break;
        }
        lastErr = GetLastError();
        if (attempt == 0) {
            LockBreaker::CloseHandlesForPath(norm, 3, nullptr, nullptr);
        } else if (attempt == 1 && opts.force) {
            std::vector<DWORD> skipped;
            for (const auto& o : HandleScanner::FindOwners(norm, &skipped, nullptr)) {
                std::wstring msg;
                LockBreaker::KillProcess(o.proc.pid, true, &msg);
                r.owners.push_back(msg);
            }
            r.level = 4;
        }
        Sleep(80);
    }
    if (!r.ok) {
        if (opts.allowReboot && RebootScheduler::ScheduleMove(norm, dest, nullptr)) {
            r.ok = true;
            r.rebootRequired = true;
            r.level = 5;
            r.message = L"改名已排队到重启后执行";
        } else {
            r.code = lastErr;
            r.level = r.level < 0 ? 2 : r.level;
            r.message = L"改名失败：" + ErrorWithCode(lastErr);
        }
    }
    batch.items.push_back(r);
    Tally(batch);
    return batch;
}

BatchResult FileOps::Move(const std::vector<std::wstring>& paths, const std::wstring& destDir,
                          const ApplyOptions& opts, const ConfirmFn& confirm) {
    BatchResult batch;
    for (const auto& p : paths) {
        ItemResult r;
        r.path = p;

        PolicyResult pol = PolicyGuard::Evaluate(p);
        if (pol.Denied()) {
            r.skipped = true;
            r.code = kExitPolicyDenied;
            r.rule = PolicyGuard::RuleName(pol.ruleId);
            r.message = pol.reason;
            batch.items.push_back(r);
            continue;
        }
        if (pol.NeedsConfirm()) {
            if (!confirm || opts.quiet) {
                r.skipped = true;
                r.code = kExitNeedConfirm;
                batch.needConfirm = true;
                r.message = L"需要二次确认，但当前无人可确认";
                batch.items.push_back(r);
                continue;
            }
            ConfirmRequest req;
            req.paths.push_back(p);
            req.rule = PolicyGuard::RuleName(pol.ruleId);
            req.reason = pol.reason;
            req.op = L"Move";
            if (!confirm(req)) {
                r.skipped = true;
                r.code = kExitNeedConfirm;
                r.message = L"用户取消了确认";
                batch.items.push_back(r);
                continue;
            }
        }

        const std::wstring src = PathResolver::Normalize(p);
        const std::wstring dst = JoinPath(destDir, BaseName(src));

        bool moved = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
        DWORD lastErr = moved ? 0 : GetLastError();

        if (!moved) {
            LockBreaker::CloseHandlesForPath(src, 3, nullptr, nullptr);
            moved = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
            if (moved) r.level = 2;
            else lastErr = GetLastError();
        }
        if (!moved && opts.force) {
            std::vector<DWORD> skipped;
            for (const auto& o : HandleScanner::FindOwners(src, &skipped, nullptr)) {
                std::wstring msg;
                LockBreaker::KillProcess(o.proc.pid, true, &msg);
            }
            Sleep(100);
            moved = MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
            if (moved) r.level = 4;
            else lastErr = GetLastError();
        }
        if (!moved && opts.allowReboot) {
            if (RebootScheduler::ScheduleMove(src, dst, nullptr)) {
                r.ok = true;
                r.rebootRequired = true;
                r.level = 5;
                r.message = L"移动已排队到重启后执行";
                batch.items.push_back(r);
                continue;
            }
        }

        if (moved) {
            r.ok = true;
            r.level = r.level < 0 ? 0 : r.level;
            r.message = L"已移动到 " + dst;
        } else {
            r.code = lastErr;
            r.message = L"移动失败：" + ErrorWithCode(lastErr);
        }
        batch.items.push_back(r);
    }
    Tally(batch);
    return batch;
}

BatchResult FileOps::Copy(const std::vector<std::wstring>& paths, const std::wstring& destDir,
                          const ApplyOptions& opts, const ConfirmFn& confirm) {
    BatchResult batch;
    for (const auto& p : paths) {
        ItemResult r;
        r.path = p;

        PolicyResult pol = PolicyGuard::Evaluate(p);
        if (pol.Denied()) {
            r.skipped = true;
            r.code = kExitPolicyDenied;
            r.rule = PolicyGuard::RuleName(pol.ruleId);
            r.message = pol.reason;
            batch.items.push_back(r);
            continue;
        }
        if (pol.NeedsConfirm()) {
            if (!confirm || opts.quiet) {
                r.skipped = true;
                r.code = kExitNeedConfirm;
                batch.needConfirm = true;
                r.message = L"需要二次确认，但当前无人可确认";
                batch.items.push_back(r);
                continue;
            }
            ConfirmRequest req;
            req.paths.push_back(p);
            req.rule = PolicyGuard::RuleName(pol.ruleId);
            req.reason = pol.reason;
            req.op = L"Copy";
            if (!confirm(req)) {
                r.skipped = true;
                r.code = kExitNeedConfirm;
                r.message = L"用户取消了确认";
                batch.items.push_back(r);
                continue;
            }
        }

        const std::wstring src = PathResolver::Normalize(p);
        const std::wstring dst = JoinPath(destDir, BaseName(src));
        std::wstring msg;
        if (CopyTree(src, dst, opts, &msg)) {
            r.ok = true;
            r.level = 0;
            r.message = L"已复制到 " + dst;
        } else {
            r.code = GetLastError();
            r.message = msg.empty() ? (L"复制失败：" + ErrorWithCode(r.code)) : msg;
        }
        batch.items.push_back(r);
    }
    Tally(batch);
    return batch;
}

}  // namespace ul
