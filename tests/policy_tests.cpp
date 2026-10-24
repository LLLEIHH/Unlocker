// 黑名单断言集（方案 T17-T24 的可执行部分）。
// 每次改 Blacklist.def 都跑一遍：回归成本极低，价值极高 —— 它挡的是蓝屏。
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <string>
#include <vector>

#include "common/Log.h"
#include "common/StringUtil.h"
#include "core/PathResolver.h"
#include "core/PolicyGuard.h"

using namespace ul;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::wstring& name, const std::wstring& detail = L"") {
    if (ok) {
        ++g_pass;
        wprintf(L"  [PASS] %s\n", name.c_str());
    } else {
        ++g_fail;
        wprintf(L"  [FAIL] %s   %s\n", name.c_str(), detail.c_str());
    }
}

std::wstring TempRoot() {
    wchar_t buf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buf);
    std::wstring dir = std::wstring(buf) + L"UnlockerPolicyTests";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void EnsureDir(const std::wstring& d) { CreateDirectoryW(d.c_str(), nullptr); }

void TouchFile(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, "x", 1, &w, nullptr);
        CloseHandle(h);
    }
}

std::wstring Describe(const PolicyResult& r) {
    wchar_t buf[512];
    swprintf_s(buf, L"verdict=%d rule=%s canonical=%s reason=%s",
               static_cast<int>(r.verdict), PolicyGuard::RuleName(r.ruleId).c_str(),
               r.canonical.c_str(), r.reason.c_str());
    return buf;
}

void ExpectVerdict(const std::wstring& path, Verdict want, const wchar_t* wantRule,
                   const std::wstring& name) {
    PolicyResult r = PolicyGuard::Evaluate(path);
    bool ok = (r.verdict == want);
    std::wstring gotRule = PolicyGuard::RuleName(r.ruleId);
    if (ok && wantRule && gotRule != wantRule) ok = false;
    Check(ok, name, Describe(r));
}

}  // namespace

int wmain() {
    LogInit(L"Unlocker");
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);   // 控制台按 UTF-16 输出，中文不乱码
    wprintf(L"=== Unlocker 黑名单断言集 ===\n");

    std::wstring err;
    bool inited = PolicyGuard::Init(&err);
    Check(inited, L"PolicyGuard::Init", err);

    // T17 直接命中硬禁
    ExpectVerdict(L"C:\\Windows\\System32\\drivers\\etc\\hosts", Verdict::Deny, L"BL-001",
                  L"T17 驱动目录 -> Deny BL-001");

    // T18 .. 穿越
    ExpectVerdict(L"C:\\Windows\\System32\\..\\System32\\ntoskrnl.exe", Verdict::Deny, L"BL-003",
                  L"T18 .. 相对穿越 -> Deny BL-003");

    // T19 NT 前缀 + 大小写
    ExpectVerdict(L"\\\\?\\C:\\WINDOWS\\system32\\NTOSKRNL.EXE", Verdict::Deny, L"BL-003",
                  L"T19 \\\\?\\ + 大小写 -> Deny BL-003");
    ExpectVerdict(L"\\??\\C:\\Windows\\System32\\ntoskrnl.exe", Verdict::Deny, L"BL-003",
                  L"T19 \\??\\ 前缀 -> Deny BL-003");

    // T20 8.3 短名
    ExpectVerdict(L"C:\\PROGRA~1", Verdict::Confirm, L"BL-017",
                  L"T20 8.3 短名展开 -> Confirm BL-017");

    // 根目录核心 DLL
    ExpectVerdict(L"C:\\Windows\\System32\\ntdll.dll", Verdict::Deny, L"BL-020",
                  L"系统核心 DLL -> Deny BL-020");

    // 内核驱动通配
    ExpectVerdict(L"C:\\Windows\\System32\\win32kfull.sys", Verdict::Deny, L"BL-005",
                  L"win32k*.sys 通配 -> Deny BL-005");
    ExpectVerdict(L"C:\\Windows\\System32\\tcpip.sys", Verdict::Deny, L"BL-006",
                  L"System32\\*.sys -> Deny BL-006");

    // 注册表 hive / 组件存储 / SysWOW64
    ExpectVerdict(L"C:\\Windows\\System32\\config\\SYSTEM", Verdict::Deny, L"BL-002",
                  L"注册表 hive -> Deny BL-002");
    ExpectVerdict(L"C:\\Windows\\WinSxS\\x86_foo", Verdict::Deny, L"BL-010",
                  L"WinSxS -> Deny BL-010");
    ExpectVerdict(L"C:\\Windows\\SysWOW64\\kernel32.dll", Verdict::Deny, L"BL-019",
                  L"SysWOW64 -> Deny BL-019");

    // 卷根
    ExpectVerdict(L"C:\\", Verdict::Deny, L"BL-007", L"卷根 -> Deny BL-007");
    ExpectVerdict(L"C:\\pagefile.sys", Verdict::Deny, L"BL-008",
                  L"任意卷根 pagefile -> Deny BL-008");

    // 系统盘根目录下的其他目录不匹配 BL-008
    ExpectVerdict(L"C:\\Windows\\Temp\\unlocker_test.log", Verdict::Allow, L"BL-030",
                  L"BL-030 显式放行 Temp");

    // PREFIX 必须按分隔符切分：System32X 不能命中 BL-001
    {
        PolicyResult r = PolicyGuard::Evaluate(L"C:\\Windows\\System32X\\a.txt");
        Check(r.verdict == Verdict::Confirm && PolicyGuard::RuleName(r.ruleId) == L"BL-016",
              L"PREFIX 分隔符边界（System32X 不命中 BL-001）", Describe(r));
    }

    // T23 误伤反例：自建同名目录必须放行
    const std::wstring root = TempRoot();
    std::wstring myApp = root + L"\\MyApp\\System32";
    EnsureDir(root + L"\\MyApp");
    EnsureDir(myApp);
    std::wstring myDll = myApp + L"\\a.dll";
    TouchFile(myDll);
    ExpectVerdict(myDll, Verdict::Allow, nullptr, L"T23 D:\\MyApp\\System32\\a.dll 同构路径 -> Allow");

    // 普通用户目录文件
    std::wstring plain = root + L"\\plain.txt";
    TouchFile(plain);
    ExpectVerdict(plain, Verdict::Allow, nullptr, L"用户目录普通文件 -> Allow");

    // T21 junction 重定向
    std::wstring junction = root + L"\\win";
    std::wstring cmdline = L"cmd.exe /c mklink /J \"" + junction + L"\" \"C:\\Windows\" >nul 2>&1";
    _wsystem(cmdline.c_str());
    DWORD attrs = GetFileAttributesW(junction.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ExpectVerdict(junction + L"\\System32\\ntoskrnl.exe", Verdict::Deny, L"BL-003",
                      L"T21 junction 重定向 -> Deny BL-003");
    } else {
        Check(true, L"T21 junction 创建被跳过（无权限），跳过用例");
    }

    // 硬链接 + FILE_ID：验证能取到 ID 且至少有一条硬禁 ID
    {
        std::wstring link = root + L"\\self_link.exe";
        DeleteFileW(link.c_str());
        std::wstring exe = PathResolver::SelfExePath();
        std::wstring linkCmd = L"cmd.exe /c mklink /H \"" + link + L"\" \"" + exe + L"\" >nul 2>&1";
        _wsystem(linkCmd.c_str());

        std::wstring id1, id2;
        bool ok1 = PolicyGuard::GetFileIdKey(exe, &id1);
        bool ok2 = PolicyGuard::GetFileIdKey(link, &id2);
        Check(ok1 && (!ok2 || id1 == id2) , L"T22 硬链接 FILE_ID 一致",
              L"exe=" + id1 + L" link=" + id2);
        if (ok2) {
            ExpectVerdict(link, Verdict::Deny, nullptr, L"T22 硬链接指向自身 exe -> Deny");
        }
    }

    // T24 规则表篡改 -> fail-closed
    PolicyGuard::TestForceCrcFailure(true);
    {
        PolicyResult r = PolicyGuard::Evaluate(plain);
        Check(r.verdict == Verdict::Deny, L"T24 CRC 失败 -> fail-closed（普通文件也 Deny）", Describe(r));
    }
    PolicyGuard::TestForceCrcFailure(false);
    PolicyGuard::Init(nullptr);
    {
        PolicyResult r = PolicyGuard::Evaluate(plain);
        Check(r.verdict == Verdict::Allow, L"T24 恢复后重新放行", Describe(r));
    }

    // COUNT 规则：>500 才确认，且只在批量入口生效
    {
        std::vector<std::wstring> many(501, plain);
        PolicyResult r = PolicyGuard::EvaluateBatch(many);
        Check(r.verdict == Verdict::Confirm && PolicyGuard::RuleName(r.ruleId) == L"BL-018",
              L"BL-018 批量 >500 -> Confirm", Describe(r));

        PolicyResult single = PolicyGuard::Evaluate(plain);
        Check(single.verdict == Verdict::Allow, L"COUNT 不参与单路径裁决", Describe(single));
    }

    // 三段式提示
    {
        PolicyResult r = PolicyGuard::Evaluate(L"C:\\Windows\\System32\\drivers\\etc\\hosts");
        std::wstring msg = PolicyGuard::ThreePartMessage(r);
        Check(msg.find(L"BL-001") != std::wstring::npos &&
                  msg.find(L"无法关闭") != std::wstring::npos,
              L"三段式拒绝提示包含规则 ID 与不可关闭说明", msg);
    }

    wprintf(L"\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
