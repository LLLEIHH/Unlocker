#pragma once
// 黑名单裁决引擎（方案 4.6）。
// 设计前提：不能指望用户在点「解锁并删除」之前想清楚那是不是 ntoskrnl.exe。
// 刹车必须做在软件里，而且不能是个能被顺手关掉的复选框。
#include <windows.h>
#include <string>
#include <vector>

namespace ul {

enum class Verdict : int { Allow = 0, Confirm = 1, Deny = 2 };
enum class Severity : int { Info = 0, High = 1, Crit = 2 };

enum class MatchType : int {
    Exact      = 0,
    Prefix     = 1,
    Glob       = 2,
    FileId     = 3,
    VolumeRoot = 4,
    Volume     = 5,
    Self       = 6,
    Count      = 7,
};

enum RuleId : int;

struct PolicyResult {
    Verdict      verdict  = Verdict::Allow;
    RuleId       ruleId{};
    Severity     severity = Severity::Info;
    std::wstring target;                 // 用户传入的原样路径（回显）
    std::wstring canonical;              // 归一化真身（日志与提示都用它）
    std::wstring reason;                 // 已本地化，可直接显示
    bool         truncated = false;      // 真身拿不到，已按 4.6.4 收紧

    bool Denied()   const { return verdict == Verdict::Deny; }
    bool NeedsConfirm() const { return verdict == Verdict::Confirm; }
};

class PolicyGuard {
public:
    // 展开占位符、建 FILE_ID 表、CRC+语义自检。任何失败 → fail-closed（全表 Deny）。
    static bool Init(std::wstring* err);
    static bool Ready();
    static bool FailClosed();

    // 单路径裁决：路径规则 + FILE_ID + 显式 Allow 优先级 + 真身缺失收紧
    static PolicyResult Evaluate(const std::wstring& rawPath);

    // 批量入口：COUNT 规则只在这里参与裁决
    static PolicyResult EvaluateBatch(const std::vector<std::wstring>& paths);

    // 进程侧裁决（4.6.5）：按完整映像路径过一遍路径规则
    static PolicyResult EvaluateProcess(const std::wstring& imageFullPath, DWORD pid);

    static std::wstring RuleName(RuleId id);          // "BL-001"
    static const wchar_t* VerdictName(Verdict v);     // 已阻止 / 需确认 / 放行

    // 三段式拒绝提示（4.6.7）
    static std::wstring ThreePartMessage(const PolicyResult& r);

    // 自检序列化：运行时 CRC 与构建期生成值比对
    static std::string SerializeRulesForCrc();

    static bool GetFileIdKey(const std::wstring& path, std::wstring* key);
    static bool GetFileIdKeyByHandle(HANDLE h, std::wstring* key);

    // 仅供 policy_tests 使用：模拟 .rdata 被 patch，验证 fail-closed
    static void TestForceCrcFailure(bool on);
};

}  // namespace ul
