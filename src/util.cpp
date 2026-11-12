#include "util.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                  nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                          nullptr);
    return out;
}

std::wstring DataDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    wchar_t* appdata = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        base = appdata;
        ::CoTaskMemFree(appdata);
    }
    if (base.empty()) base = L".";
    cached = base + L"\\TopVideoMonitor";
    ::CreateDirectoryW(cached.c_str(), nullptr);
    return cached;
}

std::wstring ConfigPath() { return DataDir() + L"\\config.ini"; }
std::wstring LogPath() { return DataDir() + L"\\error.log"; }

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring path(buf);
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring(L".") : path.substr(0, pos);
}

static std::wstring PickWritableLog() {
    const std::wstring primary = LogPath();
    HANDLE h = ::CreateFileW(primary.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        return primary;
    }
    return ExeDir() + L"\\error.log";
}

void LogLine(const std::string& line) {
    static const std::wstring path = PickWritableLog();
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char head[64];
    std::snprintf(head, sizeof(head), "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string text = std::string(head) + line + "\r\n";
    DWORD written = 0;
    ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    ::CloseHandle(h);
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring Format(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[1024];
    int n = ::_vsnwprintf_s(buf, std::size(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return L"";
    return std::wstring(buf, static_cast<size_t>(n));
}
