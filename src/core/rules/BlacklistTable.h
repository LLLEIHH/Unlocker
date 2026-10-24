#pragma once
// 展开 Blacklist.def：枚举 + constexpr 规则表（落在 .rdata，没有运行时改写入口）
#include <iterator>

#include "core/PolicyGuard.h"

namespace ul {

enum RuleId : int {
    BL_NONE = 0,
#define BL_RULE_ID(id) id,
#define BL_RULE(id, m, p, v, s, r)
#include "core/rules/Blacklist.def"
#undef BL_RULE
#undef BL_RULE_ID
    BL_RULE_ID_MAX
};

struct RuleRow {
    RuleId        id;
    MatchType     type;
    const wchar_t* pattern;
    Verdict       verdict;
    Severity      severity;
    const wchar_t* reason;
};

// .def 里用 MATCH_*/ALLOW|CONFIRM|DENY/INFO|HIGH|CRIT 这些「规则语言」词，
// 这里映射到 C++ 枚举，保持规则表可读且与实现解耦。
#define UL_MATCH_EXACT       MatchType::Exact
#define UL_MATCH_PREFIX      MatchType::Prefix
#define UL_MATCH_GLOB        MatchType::Glob
#define UL_MATCH_FILE_ID     MatchType::FileId
#define UL_MATCH_VOLUME_ROOT MatchType::VolumeRoot
#define UL_MATCH_VOLUME      MatchType::Volume
#define UL_MATCH_SELF        MatchType::Self
#define UL_MATCH_COUNT       MatchType::Count

#define UL_ALLOW   Verdict::Allow
#define UL_CONFIRM Verdict::Confirm
#define UL_DENY    Verdict::Deny

#define UL_INFO Severity::Info
#define UL_HIGH Severity::High
#define UL_CRIT Severity::Crit

inline constexpr RuleRow kRules[] = {
#define BL_RULE_ID(id)
#define BL_RULE(id, m, p, v, s, r) { RuleId::id, UL_##m, p, UL_##v, UL_##s, L"" r },
#include "core/rules/Blacklist.def"
#undef BL_RULE
#undef BL_RULE_ID
};

inline constexpr size_t kRuleCount = std::size(kRules);

inline constexpr const wchar_t* kRuleNames[] = {
#define BL_RULE_ID(id) L"" #id,
#define BL_RULE(id, m, p, v, s, r)
#include "core/rules/Blacklist.def"
#undef BL_RULE
#undef BL_RULE_ID
};

}  // namespace ul
