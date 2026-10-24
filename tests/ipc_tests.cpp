// IPC 协议断言集（M4 的自检，等效于 T35/T36/T38 的可自动化部分）。
// 真机 UAC 弹窗无法在自动化里点「是」，所以这里用 PipeServer::SetTestMode 跳过身份校验，
// 专注验证：hello 版本握手、op 白名单、畸形帧、worker 端重新裁决（黑名单）。
#include <windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <string>
#include <thread>
#include <vector>

#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"
#include "ipc/PipeServer.h"
#include "ipc/Protocol.h"

using namespace ul;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::wstring& name, const std::wstring& detail = L"") {
    if (ok) { ++g_pass; wprintf(L"  [PASS] %s\n", name.c_str()); }
    else    { ++g_fail; wprintf(L"  [FAIL] %s   %s\n", name.c_str(), detail.c_str()); }
}

std::wstring RandomPipeName() {
    static const wchar_t* hex = L"0123456789abcdef";
    std::wstring name = L"\\\\.\\pipe\\Unlocker.W.";
    srand(GetTickCount());
    for (int i = 0; i < 32; ++i) name.push_back(hex[rand() % 16]);
    return name;
}

bool ConnectClient(const std::wstring& name, HANDLE* out) {
    for (int i = 0; i < 50; ++i) {
        if (WaitNamedPipeW(name.c_str(), 100)) {
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD mode = PIPE_READMODE_MESSAGE;
                SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
                *out = h;
                return true;
            }
        }
        Sleep(50);
    }
    return false;
}

json::Value MakeHello(uint64_t id, int ver) {
    json::Value v = json::Value::MakeObject();
    v.Set("id", json::Value::MakeNumber(static_cast<double>(id)));
    v.Set("op", json::Value::MakeString("hello"));
    v.Set("ver", json::Value::MakeNumber(ver));
    return v;
}

bool SendRaw(HANDLE h, const std::string& payload) {
    return WriteFrame(h, payload, 5000);
}

bool Recv(HANDLE h, json::Value* v, DWORD timeoutMs = 20000) {
    std::string frame;
    if (!ReadFrame(h, &frame, timeoutMs)) return false;
    std::string err;
    return json::Parse(frame, v, &err);
}

// 启动一个 worker 线程（测试模式），返回客户端句柄；version 用于握手
struct TestServer {
    std::wstring name;
    std::thread  thread;
    HANDLE       client = nullptr;

    bool Start() {
        name = RandomPipeName();
        WorkerContext ctx;
        ctx.pipeName = name;
        ctx.expectedUiPid = GetCurrentProcessId();
        PipeServer::SetTestMode(true);
        thread = std::thread([ctx] { PipeServer::Run(ctx); });
        return ConnectClient(name, &client);
    }
    void Stop(HANDLE h) {
        if (thread.joinable()) thread.join();
        (void)h;
    }
};

std::wstring TempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    std::wstring p = std::wstring(dir) + L"UnlockerIpcTests";
    CreateDirectoryW(p.c_str(), nullptr);
    return p + L"\\" + name;
}

}  // namespace

int wmain() {
    LogInit(L"Unlocker");
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    wprintf(L"=== Unlocker IPC 协议断言集 ===\n");

    PathResolver::Init();
    std::wstring perr;
    PolicyGuard::Init(&perr);

    // ---------------- 用例 1：正常握手 + ping + scan
    {
        TestServer srv;
        Check(srv.Start(), L"worker 建管道（含 SACL ML;;NW;;;ME）并接受连接");
        if (srv.client) {
            Check(SendRaw(srv.client, json::Dump(MakeHello(1, 3))), L"发送 hello");
            json::Value ack;
            Check(Recv(srv.client, &ack) && ack.GetBool("ok", false) &&
                      static_cast<int>(ack.GetNumber("ver", 0)) == 3,
                  L"握手回包 ver=3");

            json::Value ping = json::Value::MakeObject();
            ping.Set("id", json::Value::MakeNumber(2));
            ping.Set("op", json::Value::MakeString("ping"));
            SendRaw(srv.client, json::Dump(ping));
            json::Value pong;
            Check(Recv(srv.client, &pong) && pong.GetString("evt") == "pong", L"ping -> pong");

            // 扫描一个被外部进程锁住的文件
            const std::wstring target = TempFile(L"ipc_locked.txt");
            DeleteFileW(target.c_str());
            HANDLE f = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(f, "x", 1, &w, nullptr);
                CloseHandle(f);
            }
            std::wstring cmd = L"cmd.exe /c start /b \"\" \"" +
                               (PathResolver::SelfDir() + L"\\MakeLock.exe") + L"\" \"" + target + L"\" 15";
            _wsystem(cmd.c_str());

            // 等 MakeLock 真正拿到独占锁再扫描。原来固定 Sleep(600) 在 CI 冷机上偶发失败
            // （子进程还没打开文件就扫了），这里改成轮询：自己打不开文件即说明锁已生效。
            bool lockedByOther = false;
            for (int i = 0; i < 50 && !lockedByOther; ++i) {
                Sleep(100);
                HANDLE probe = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
                if (probe == INVALID_HANDLE_VALUE) {
                    lockedByOther = true;
                } else {
                    CloseHandle(probe);
                }
            }
            Check(lockedByOther, L"MakeLock 已独占目标文件（轮询确认）");

            json::Value scan = json::Value::MakeObject();
            scan.Set("id", json::Value::MakeNumber(3));
            scan.Set("op", json::Value::MakeString("scan"));
            json::Value items = json::Value::MakeArray();
            json::Value it = json::Value::MakeObject();
            it.Set("path", json::Value::MakeString(Utf16ToUtf8(target)));
            items.arr.push_back(it);
            scan.Set("items", std::move(items));
            SendRaw(srv.client, json::Dump(scan));

            bool sawProgress = false;
            json::Value result;
            bool gotFinal = false;
            for (int i = 0; i < 8; ++i) {
                if (!Recv(srv.client, &result)) break;
                if (result.GetString("evt") == "progress") { sawProgress = true; continue; }
                gotFinal = true;
                break;
            }
            const json::Value* results = result.Find("results");
            bool hasLocked = false;
            std::wstring seen = L"(没拿到最终结果)";
            if (results && results->type == json::Value::Type::Array && !results->arr.empty()) {
                seen = L"state=" + Utf8ToUtf16(results->arr[0].GetString("state"));
                hasLocked = results->arr[0].GetString("state") == "locked";
            } else if (gotFinal) {
                seen = L"(results 数组为空)";
            }
            Check(gotFinal && hasLocked, L"scan 返回 locked 状态", seen);
            Check(sawProgress || gotFinal, L"scan 有进度或最终结果");

            // worker 端重新裁决：删系统文件必须被 BL-001/003 挡下
            json::Value del = json::Value::MakeObject();
            del.Set("id", json::Value::MakeNumber(4));
            del.Set("op", json::Value::MakeString("delete"));
            json::Value ditems = json::Value::MakeArray();
            json::Value ditem = json::Value::MakeObject();
            ditem.Set("path", json::Value::MakeString(Utf16ToUtf8(L"C:\\Windows\\System32\\drivers\\etc\\hosts")));
            ditems.arr.push_back(ditem);
            del.Set("items", std::move(ditems));
            SendRaw(srv.client, json::Dump(del));

            json::Value delResult;
            bool denied = false;
            if (Recv(srv.client, &delResult)) {
                denied = static_cast<int>(delResult.GetNumber("code", 0)) == kExitPolicyDenied;
                const json::Value* rr = delResult.Find("results");
                if (rr && rr->type == json::Value::Type::Array && !rr->arr.empty()) {
                    const std::string rule = rr->arr[0].GetString("rule");
                    denied = denied && (rule == "BL-001" || rule == "BL-003");
                }
            }
            Check(denied, L"worker 重校验：删除系统文件被黑名单拒绝（code=4）");

            // 未知 op：拒绝并断连
            json::Value bad = json::Value::MakeObject();
            bad.Set("id", json::Value::MakeNumber(5));
            bad.Set("op", json::Value::MakeString("exec"));
            SendRaw(srv.client, json::Dump(bad));
            json::Value badResult;
            const bool gotErr = Recv(srv.client, &badResult);
            Check(gotErr && !badResult.GetBool("ok", true), L"未知 op 被拒绝（不存在 exec 入口）");

            // 断连后 worker 应退出
            CloseHandle(srv.client);
            srv.client = nullptr;
            srv.Stop(nullptr);
        }
    }

    // ---------------- 用例 2：协议版本不匹配
    {
        TestServer srv;
        if (srv.Start()) {
            SendRaw(srv.client, json::Dump(MakeHello(1, 2)));
            json::Value ack;
            bool rejected = false;
            if (Recv(srv.client, &ack)) rejected = !ack.GetBool("ok", true);
            Check(rejected, L"hello ver=2 被拒绝（版本不匹配）");
            CloseHandle(srv.client);
            srv.client = nullptr;
            srv.Stop(nullptr);
        }
    }

    // ---------------- 用例 3：畸形帧（长度头 > 16MB）
    {
        TestServer srv;
        if (srv.Start()) {
            SendRaw(srv.client, json::Dump(MakeHello(1, 3)));
            json::Value ack;
            Recv(srv.client, &ack);

            const uint32_t huge = 0xFFFFFFFF;
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            WriteFile(srv.client, &huge, 4, nullptr, &ov);
            WaitForSingleObject(ov.hEvent, 1000);
            CloseHandle(ov.hEvent);

            // 服务端应断连：之后的读会失败
            std::string frame;
            const bool disconnected = !ReadFrame(srv.client, &frame, 3000);
            Check(disconnected, L"超长长度头被拒绝并断连");
            CloseHandle(srv.client);
            srv.client = nullptr;
            srv.Stop(nullptr);
        }
    }

    wprintf(L"\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
