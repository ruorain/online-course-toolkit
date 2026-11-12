#include "app.h"

#include <shlobj.h>

#include <cstdio>
#include <map>
#include <string>

#include "util.h"

AppState g_app;

namespace {

std::string ReadAllUtf8(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    std::string data;
    if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!::ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) {
            data.clear();
        } else {
            data.resize(read);
        }
    }
    ::CloseHandle(h);
    // 去掉 UTF-8 BOM
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return data;
}

bool WriteAllUtf8(const std::wstring& path, const std::string& data) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0;
    ::CloseHandle(h);
    return ok;
}

std::map<std::wstring, std::wstring> ParseIni(const std::string& text) {
    std::map<std::wstring, std::wstring> kv;
    std::wstring section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        std::wstring wline = Utf8ToWide(line);
        if (wline.size() >= 2 && wline.front() == L'[' && wline.back() == L']') {
            section = wline.substr(1, wline.size() - 2);
            continue;
        }
        size_t eq = wline.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(wline.substr(0, eq));
        std::wstring val = Trim(wline.substr(eq + 1));
        kv[section + L"/" + key] = val;
    }
    return kv;
}

int ToInt(const std::wstring& s, int def) {
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    long v = std::wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return static_cast<int>(v);
}

bool FileExists(const std::wstring& path) {
    DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::wstring FindBrowser() {
    const wchar_t* candidates[] = {
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%LOCALAPPDATA%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
        L"%LOCALAPPDATA%\\Google\\Chrome\\Application\\chrome.exe",
    };
    for (const wchar_t* tpl : candidates) {
        wchar_t expanded[MAX_PATH * 2]{};
        if (!::ExpandEnvironmentStringsW(tpl, expanded, static_cast<DWORD>(std::size(expanded)))) {
            continue;
        }
        if (FileExists(expanded)) return expanded;
    }
    return std::wstring();
}

std::wstring DefaultProfileDir() {
    wchar_t* local = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        base = local;
        ::CoTaskMemFree(local);
    }
    if (base.empty()) base = L".";
    std::wstring dir = base + L"\\TopVideoMonitor\\browser-profile";
    return dir;
}

static std::wstring PortableConfigPath() { return ExeDir() + L"\\config.ini"; }

static std::string ReadConfigText() {
    std::string t = ReadAllUtf8(ConfigPath());
    if (t.empty()) t = ReadAllUtf8(PortableConfigPath());
    return t;
}

AppConfig LoadConfig() {
    AppConfig cfg;
    auto kv = ParseIni(ReadConfigText());
    auto get = [&](const wchar_t* key, const wchar_t* def) -> std::wstring {
        auto it = kv.find(key);
        return it == kv.end() ? std::wstring(def) : it->second;
    };

    cfg.port = ToInt(get(L"general/port", L"9222"), 9222);
    cfg.browser = get(L"general/browser", L"");
    cfg.profile = get(L"general/profile", L"");
    cfg.thresholdSec = ToInt(get(L"alert/threshold", L"5"), 5);
    cfg.flashMs = ToInt(get(L"alert/flash", L"450"), 450);
    cfg.beepSec = ToInt(get(L"alert/beep", L"8"), 8);
    cfg.sound = ToInt(get(L"alert/sound", L"1"), 1) != 0;
    cfg.fontPx = ToInt(get(L"bar/font", L"22"), 22);
    cfg.alpha = ToInt(get(L"bar/alpha", L"100"), 100);
    cfg.barX = ToInt(get(L"bar/x", L"-99999"), -99999);
    cfg.barY = ToInt(get(L"bar/y", L"-99999"), -99999);
    for (int i = 0; i < 2; ++i) {
        std::wstring sec = i == 0 ? L"ch1/" : L"ch2/";
        cfg.ch[i].label = get((sec + L"label").c_str(), i == 0 ? L"视频1" : L"视频2");
        cfg.ch[i].url = get((sec + L"url").c_str(), L"");
        cfg.ch[i].title = get((sec + L"title").c_str(), L"");
    }

    if (cfg.thresholdSec < 1) cfg.thresholdSec = 1;
    if (cfg.flashMs < 120) cfg.flashMs = 120;
    if (cfg.beepSec < 2) cfg.beepSec = 2;
    if (cfg.fontPx < 12) cfg.fontPx = 12;
    if (cfg.fontPx > 48) cfg.fontPx = 48;
    if (cfg.alpha < 35) cfg.alpha = 35;
    if (cfg.alpha > 100) cfg.alpha = 100;
    if (cfg.browser.empty()) cfg.browser = FindBrowser();
    if (cfg.profile.empty()) cfg.profile = DefaultProfileDir();
    return cfg;
}

void SaveConfig(const AppConfig& cfg) {
    std::string out;
    out += "; TopVideoMonitor 配置（程序自动生成）\r\n";
    out += "[general]\r\n";
    out += "port=" + std::to_string(cfg.port) + "\r\n";
    out += "browser=" + WideToUtf8(cfg.browser) + "\r\n";
    out += "profile=" + WideToUtf8(cfg.profile) + "\r\n";
    out += "[alert]\r\n";
    out += "threshold=" + std::to_string(cfg.thresholdSec) + "\r\n";
    out += "flash=" + std::to_string(cfg.flashMs) + "\r\n";
    out += "beep=" + std::to_string(cfg.beepSec) + "\r\n";
    out += std::string("sound=") + (cfg.sound ? "1" : "0") + "\r\n";
    out += "[bar]\r\n";
    out += "x=" + std::to_string(cfg.barX) + "\r\n";
    out += "y=" + std::to_string(cfg.barY) + "\r\n";
    out += "font=" + std::to_string(cfg.fontPx) + "\r\n";
    out += "alpha=" + std::to_string(cfg.alpha) + "\r\n";
    for (int i = 0; i < 2; ++i) {
        out += std::string(i == 0 ? "[ch1]\r\n" : "[ch2]\r\n");
        out += "label=" + WideToUtf8(cfg.ch[i].label) + "\r\n";
        out += "url=" + WideToUtf8(cfg.ch[i].url) + "\r\n";
        out += "title=" + WideToUtf8(cfg.ch[i].title) + "\r\n";
    }
    if (!WriteAllUtf8(ConfigPath(), out)) {
        // %APPDATA% 写不了（便携/受控环境）就写 exe 旁边
        WriteAllUtf8(PortableConfigPath(), out);
    }
}

AppConfig ConfigCopy() {
    std::lock_guard<std::mutex> lock(g_app.cfgMutex);
    return g_app.cfg;
}

double NowSeconds() {
    static LARGE_INTEGER freq{};
    static LARGE_INTEGER start{};
    if (freq.QuadPart == 0) {
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
}
