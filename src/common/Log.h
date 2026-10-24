#pragma once
// 环形日志：写 %LOCALAPPDATA%\Unlocker\unlocker.log，超出 2MB 轮转一次。
// 审计行（DENY / CONFIRM）与普通诊断行共用一套写入点，格式见方案 4.6.8。
#include <string>

namespace ul {

enum class LogLevel { Debug, Info, Warn, Error };

void LogInit(const std::wstring& appName = L"Unlocker");
void LogWrite(LogLevel level, const std::wstring& msg);
std::wstring LogFilePath();

inline void LogI(const std::wstring& m) { LogWrite(LogLevel::Info, m); }
inline void LogW(const std::wstring& m) { LogWrite(LogLevel::Warn, m); }
inline void LogE(const std::wstring& m) { LogWrite(LogLevel::Error, m); }

// 审计：rule= 与 caller= 必须同时记，否则线上分不清是谁触发的（4.6.8）
void LogAudit(const wchar_t* tag, const std::wstring& rule, const std::wstring& target,
              const std::wstring& canonical, const std::wstring& op, const std::wstring& caller);

}  // namespace ul
