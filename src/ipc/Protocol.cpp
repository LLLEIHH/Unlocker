#include "ipc/Protocol.h"

#include <algorithm>
#include <cstring>

#include "common/ErrorMap.h"
#include "common/StringUtil.h"

namespace ul {
namespace {

bool OverlappedIo(HANDLE h, bool isWrite, void* buf, DWORD n, DWORD timeoutMs, DWORD* transferred) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    BOOL ok = isWrite ? WriteFile(h, buf, n, nullptr, &ov) : ReadFile(h, buf, n, nullptr, &ov);
    if (!ok) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            SetLastError(e);
            return false;
        }
        const DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            WaitForSingleObject(ov.hEvent, 1000);
            CloseHandle(ov.hEvent);
            SetLastError(ERROR_TIMEOUT);
            return false;
        }
        DWORD got = 0;
        if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
        if (transferred) *transferred = got;
        CloseHandle(ov.hEvent);
        return true;
    }

    DWORD got = 0;
    GetOverlappedResult(h, &ov, &got, FALSE);
    if (transferred) *transferred = got;
    CloseHandle(ov.hEvent);
    return true;
}

std::string WideToUtf8(const std::wstring& s) { return Utf16ToUtf8(s); }

}  // namespace

const char* OpName(IpcOp op) {
    switch (op) {
        case IpcOp::Hello:    return "hello";
        case IpcOp::Ping:     return "ping";
        case IpcOp::Scan:     return "scan";
        case IpcOp::Unlock:   return "unlock";
        case IpcOp::Delete:   return "delete";
        case IpcOp::Rename:   return "rename";
        case IpcOp::Move:     return "move";
        case IpcOp::Copy:     return "copy";
        case IpcOp::Reboot:   return "reboot";
        case IpcOp::Cancel:   return "cancel";
        case IpcOp::Shutdown: return "shutdown";
        default:              return "invalid";
    }
}

IpcOp ParseOp(const std::string& name) {
    // 白名单式：任何其它 op 一律拒绝并记日志，不存在 exec/shell/run 这类入口
    if (name == "hello")    return IpcOp::Hello;
    if (name == "ping")     return IpcOp::Ping;
    if (name == "scan")     return IpcOp::Scan;
    if (name == "unlock")   return IpcOp::Unlock;
    if (name == "delete")   return IpcOp::Delete;
    if (name == "rename")   return IpcOp::Rename;
    if (name == "move")     return IpcOp::Move;
    if (name == "copy")     return IpcOp::Copy;
    if (name == "reboot")   return IpcOp::Reboot;
    if (name == "cancel")   return IpcOp::Cancel;
    if (name == "shutdown") return IpcOp::Shutdown;
    return IpcOp::Invalid;
}

bool IsDangerousOp(IpcOp op) {
    switch (op) {
        case IpcOp::Unlock:
        case IpcOp::Delete:
        case IpcOp::Rename:
        case IpcOp::Move:
        case IpcOp::Copy:
        case IpcOp::Reboot:
            return true;
        default:
            return false;
    }
}

std::string EncodeRequest(const IpcRequest& req) {
    json::Value root = json::Value::MakeObject();
    root.Set("id", json::Value::MakeNumber(static_cast<double>(req.id)));
    root.Set("op", json::Value::MakeString(OpName(req.op)));

    json::Value items = json::Value::MakeArray();
    for (const auto& p : req.items) {
        json::Value it = json::Value::MakeObject();
        it.Set("path", json::Value::MakeString(WideToUtf8(p)));
        items.arr.push_back(std::move(it));
    }
    root.Set("items", std::move(items));
    if (!req.arg.empty()) root.Set("arg", json::Value::MakeString(WideToUtf8(req.arg)));
    if (req.force)  root.Set("force", json::Value::MakeBool(true));
    if (req.reboot) root.Set("reboot", json::Value::MakeBool(true));
    if (req.quiet)  root.Set("quiet", json::Value::MakeBool(true));
    if (!req.caller.empty()) root.Set("caller", json::Value::MakeString(req.caller));
    return json::Dump(root);
}

bool DecodeRequest(const std::string& text, IpcRequest* req, std::string* err) {
    if (text.size() > kMaxMessageBytes) {
        if (err) *err = "message too large";
        return false;
    }
    json::Value root;
    if (!json::Parse(text, &root, err)) return false;
    if (root.type != json::Value::Type::Object) {
        if (err) *err = "not an object";
        return false;
    }

    req->id    = static_cast<uint64_t>(root.GetNumber("id", 0));
    req->op    = ParseOp(root.GetString("op"));
    req->force = root.GetBool("force", false);
    req->reboot = root.GetBool("reboot", false);
    req->quiet = root.GetBool("quiet", false);
    req->arg   = Utf8ToUtf16(root.GetString("arg"));
    const std::string caller = root.GetString("caller");
    if (!caller.empty()) req->caller = caller;

    req->items.clear();
    const json::Value* items = root.Find("items");
    if (items && items->type == json::Value::Type::Array) {
        for (const auto& it : items->arr) {
            // 路径一律放在 items[].path，不存在 paths 这种写法
            if (it.type != json::Value::Type::Object) continue;
            std::wstring p = Utf8ToUtf16(it.GetString("path"));
            if (!p.empty()) req->items.push_back(std::move(p));
        }
    }
    return true;
}

bool ReadFrame(HANDLE pipe, std::string* out, DWORD timeoutMs) {
    uint32_t len = 0;
    DWORD got = 0;
    if (!OverlappedIo(pipe, false, &len, 4, timeoutMs, &got) || got != 4) return false;
    if (len == 0 || len > kMaxMessageBytes) {
        SetLastError(ERROR_FILE_INVALID);   // 畸形请求：直接断连
        return false;
    }
    out->assign(len, '\0');
    DWORD total = 0;
    while (total < len) {
        DWORD part = 0;
        if (!OverlappedIo(pipe, false, out->data() + total, len - total, timeoutMs, &part) || part == 0) {
            return false;
        }
        total += part;
    }
    return true;
}

bool WriteFrame(HANDLE pipe, const std::string& jsonStr, DWORD timeoutMs) {
    if (jsonStr.size() > kMaxMessageBytes) {
        SetLastError(ERROR_FILE_INVALID);
        return false;
    }
    const uint32_t len = static_cast<uint32_t>(jsonStr.size());
    DWORD wrote = 0;
    if (!OverlappedIo(pipe, true, const_cast<uint32_t*>(&len), 4, timeoutMs, &wrote) || wrote != 4) {
        return false;
    }
    size_t total = 0;
    while (total < jsonStr.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(jsonStr.size() - total, 64 * 1024));
        if (!OverlappedIo(pipe, true, const_cast<char*>(jsonStr.data() + total), chunk, timeoutMs, &wrote) ||
            wrote == 0) {
            return false;
        }
        total += wrote;
    }
    return true;
}

const char* StateToken(OccupancyState s) {
    switch (s) {
        case OccupancyState::Free:           return "free";
        case OccupancyState::Opened:         return "opened";
        case OccupancyState::Locked:         return "locked";
        case OccupancyState::ImageMapped:    return "imagemapped";
        case OccupancyState::AccessDenied:   return "accessdenied";
        case OccupancyState::LockedByKernel: return "lockedbykernel";
        case OccupancyState::NotFound:       return "notfound";
        case OccupancyState::Error:          return "error";
    }
    return "error";
}

const char* OpCodeName(IpcOp op) { return OpName(op); }

namespace proto {

json::Value ReportToJson(const FileReport& rep) {
    json::Value o = json::Value::MakeObject();
    o.Set("path", json::Value::MakeString(Utf16ToUtf8(rep.path)));
    o.Set("canonical", json::Value::MakeString(Utf16ToUtf8(rep.canonical)));
    o.Set("state", json::Value::MakeString(StateToken(rep.state)));
    o.Set("stateText", json::Value::MakeString(Utf16ToUtf8(OccupancyText(rep.state))));
    o.Set("error", json::Value::MakeNumber(rep.error));
    o.Set("errorText", json::Value::MakeString(Utf16ToUtf8(Win32Message(rep.error))));
    o.Set("size", json::Value::MakeNumber(static_cast<double>(rep.size)));
    o.Set("dir", json::Value::MakeBool(rep.directory));
    o.Set("needElevation", json::Value::MakeBool(rep.needElevation));
    o.Set("handles", json::Value::MakeNumber(rep.totalFileHandles));

    json::Value owners = json::Value::MakeArray();
    std::vector<DWORD> seen;
    for (const auto& ow : rep.owners) {
        bool dup = false;
        for (DWORD p : seen) if (p == ow.proc.pid) { dup = true; break; }
        if (dup) continue;   // 进程维度聚合：同进程多句柄只出一行，数量在 handles 里
        seen.push_back(ow.proc.pid);

        json::Value po = json::Value::MakeObject();
        po.Set("pid", json::Value::MakeNumber(ow.proc.pid));
        po.Set("name", json::Value::MakeString(Utf16ToUtf8(ow.proc.name)));
        po.Set("fullPath", json::Value::MakeString(Utf16ToUtf8(ow.proc.fullPath)));
        po.Set("desc", json::Value::MakeString(Utf16ToUtf8(ow.proc.description)));
        po.Set("company", json::Value::MakeString(Utf16ToUtf8(ow.proc.company)));
        po.Set("handles", json::Value::MakeNumber(ow.proc.handleCount));
        po.Set("protected", json::Value::MakeBool(ow.proc.protectedProc));
        po.Set("elevated", json::Value::MakeBool(ow.proc.elevated));
        po.Set("image", json::Value::MakeBool(ow.isImageMapping));
        owners.arr.push_back(std::move(po));
    }
    o.Set("owners", std::move(owners));

    json::Value skipped = json::Value::MakeArray();
    for (DWORD pid : rep.skippedPids) skipped.arr.push_back(json::Value::MakeNumber(pid));
    o.Set("skippedPids", std::move(skipped));
    return o;
}

json::Value ItemResultToJson(const ItemResult& r) {
    json::Value o = json::Value::MakeObject();
    o.Set("path", json::Value::MakeString(Utf16ToUtf8(r.path)));
    o.Set("ok", json::Value::MakeBool(r.ok));
    o.Set("skipped", json::Value::MakeBool(r.skipped));
    o.Set("code", json::Value::MakeNumber(r.code));
    o.Set("level", json::Value::MakeNumber(r.level));
    o.Set("reboot", json::Value::MakeBool(r.rebootRequired));
    o.Set("rule", json::Value::MakeString(Utf16ToUtf8(r.rule)));
    o.Set("msg", json::Value::MakeString(Utf16ToUtf8(r.message)));
    o.Set("closed", json::Value::MakeNumber(r.closedHandles));
    json::Value owners = json::Value::MakeArray();
    for (const auto& s : r.owners) owners.arr.push_back(json::Value::MakeString(Utf16ToUtf8(s)));
    o.Set("owners", std::move(owners));
    return o;
}

json::Value ProgressEvent(uint64_t id, int cur, int total, const std::wstring& stage) {
    json::Value o = json::Value::MakeObject();
    o.Set("id", json::Value::MakeNumber(static_cast<double>(id)));
    o.Set("evt", json::Value::MakeString("progress"));
    o.Set("cur", json::Value::MakeNumber(cur));
    o.Set("total", json::Value::MakeNumber(total));
    o.Set("stage", json::Value::MakeString(Utf16ToUtf8(stage)));
    return o;
}

json::Value ErrorResponse(uint64_t id, int code, const std::wstring& msg) {
    json::Value o = json::Value::MakeObject();
    o.Set("id", json::Value::MakeNumber(static_cast<double>(id)));
    o.Set("ok", json::Value::MakeBool(false));
    o.Set("code", json::Value::MakeNumber(code));
    o.Set("msg", json::Value::MakeString(Utf16ToUtf8(msg)));
    return o;
}

}  // namespace proto
}  // namespace ul
