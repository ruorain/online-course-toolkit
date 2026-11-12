// 杂项：UTF-8/UTF-16 转换、路径、日志。
#pragma once

#include <string>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// %APPDATA%\TopVideoMonitor
std::wstring DataDir();
std::wstring ConfigPath();
std::wstring LogPath();
std::wstring ExeDir();

void LogLine(const std::string& line);
void LogError(const char* where);

std::wstring Trim(const std::wstring& s);
std::wstring Format(const wchar_t* fmt, ...);
