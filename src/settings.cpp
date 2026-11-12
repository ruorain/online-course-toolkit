#include "app.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "resource.h"
#include "util.h"

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);

namespace {

struct ScanResult {
    bool connected = false;
    std::wstring version;
    std::vector<PageInfo> pages;
};

HWND g_dlg = nullptr;
std::atomic<bool> g_dialogAlive{false};
std::vector<PageInfo> g_pages;
std::vector<int> g_order;  // listbox 行号 → g_pages 下标

void UpdateBindInfo(HWND dlg) {
    AppConfig cfg = ConfigCopy();
    std::wstring text = L"当前绑定：";
    for (int i = 0; i < 2; ++i) {
        if (i) text += L"      ";
        text += cfg.ch[i].label.empty() ? (i == 0 ? L"视频1" : L"视频2") : cfg.ch[i].label;
        text += L" ← ";
        text += cfg.ch[i].bound() ? (cfg.ch[i].title.empty() ? L"(已绑定)" : cfg.ch[i].title)
                                  : L"（未指定）";
    }
    ::SetDlgItemTextW(dlg, IDC_BINDINFO, text.c_str());
}

void FillList(HWND dlg, const ScanResult& r) {
    HWND list = ::GetDlgItem(dlg, IDC_LIST);
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    g_pages = r.pages;
    g_order.clear();

    ::SetDlgItemTextW(dlg, IDC_CONN,
                      r.connected ? (L"已连接：" + r.version + L" · 端口可用").c_str()
                                  : L"未连接 · 请点「启动受控浏览器」");

    // 按窗口分组编号
    std::vector<std::pair<int, int>> winIds;
    for (const auto& p : g_pages) winIds.push_back({p.windowId, 0});
    int windowNo = 0;
    std::vector<int> assigned(g_pages.size(), -1);
    for (size_t i = 0; i < g_pages.size(); ++i) {
        if (assigned[i] >= 0) continue;
        ++windowNo;
        for (size_t j = i; j < g_pages.size(); ++j) {
            if (assigned[j] < 0 && g_pages[j].windowId == g_pages[i].windowId) assigned[j] = windowNo;
        }
    }

    for (size_t i = 0; i < g_pages.size(); ++i) {
        const PageInfo& p = g_pages[i];
        std::wstring videos = p.videos < 0 ? L"…" : std::to_wstring(p.videos) + L"个视频";
        std::wstring line = L"窗口" + std::to_wstring(assigned[i]) + L"   " + Utf8ToWide(p.title) +
                            L"   [" + videos + L"]";
        ::SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        g_order.push_back((int)i);
    }

    std::wstring state = L"扫描完成：" + std::to_wstring(g_pages.size()) + L" 个标签页";
    int withVideo = 0;
    for (const auto& p : g_pages) {
        if (p.videos > 0) ++withVideo;
    }
    if (withVideo < 2) {
        state += L"（只有 " + std::to_wstring(withVideo) +
                 L" 个带视频的页面，点「自动指定」可能不够两个）";
    }
    ::SetDlgItemTextW(dlg, IDC_STATE, state.c_str());
    UpdateBindInfo(dlg);
}

void StartScan(HWND dlg) {
    ::SetDlgItemTextW(dlg, IDC_STATE, L"正在扫描窗口和视频，请稍等…");
    std::thread([dlg] {
        auto* r = new ScanResult();
        AppConfig cfg = ConfigCopy();
        BrowserSnapshot snap = FetchBrowser(cfg.port, true, 1600);
        r->connected = snap.ok;
        r->version = Utf8ToWide(snap.version);
        r->pages = snap.pages;
        unsigned long long deadline = ::GetTickCount64() + 6000;
        for (auto& p : r->pages) {
            if (::GetTickCount64() > deadline) break;
            if (p.ws.empty()) continue;
            Cdp cdp;
            std::string err;
            if (!cdp.Connect(p.ws, 1200, &err)) continue;
            FrameProber prober(cdp);
            p.videos = CountVideos(prober, 1200);
            cdp.Close();
        }
        if (!::PostMessageW(dlg, WM_APP_PAGES, 0, (LPARAM)r)) delete r;
    }).detach();
}

int SelectedPage() {
    int sel = (int)::SendDlgItemMessageW(g_dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_order.size()) return -1;
    return g_order[sel];
}

void BindChannel(HWND dlg, int ch) {
    int idx = SelectedPage();
    if (idx < 0) {
        ::MessageBoxW(dlg, L"请先在列表里选中一个窗口。", L"提示", MB_ICONINFORMATION | MB_OK);
        return;
    }
    const PageInfo& p = g_pages[idx];
    AppConfig cfg = ConfigCopy();
    cfg.ch[ch].label = ch == 0 ? L"视频1" : L"视频2";
    cfg.ch[ch].url = Utf8ToWide(p.url);
    cfg.ch[ch].title = Utf8ToWide(p.title);
    {
        std::lock_guard<std::mutex> lock(g_app.cfgMutex);
        g_app.cfg.ch[ch] = cfg.ch[ch];
    }
    g_app.reloadBindings.store(true);
    SaveConfig(cfg);
    UpdateBindInfo(dlg);
}

void AutoBind(HWND dlg) {
    std::vector<int> chosen;
    std::vector<std::pair<int, int>> cand;  // (videos, index)
    for (size_t i = 0; i < g_pages.size(); ++i) {
        if (g_pages[i].videos > 0) cand.push_back({g_pages[i].videos, (int)i});
    }
    std::sort(cand.begin(), cand.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    // 先保证不同窗口
    std::vector<int> usedWindows;
    for (const auto& c : cand) {
        if (chosen.size() >= 2) break;
        int wid = g_pages[c.second].windowId;
        if (std::find(usedWindows.begin(), usedWindows.end(), wid) != usedWindows.end()) continue;
        usedWindows.push_back(wid);
        chosen.push_back(c.second);
    }
    for (const auto& c : cand) {
        if (chosen.size() >= 2) break;
        if (std::find(chosen.begin(), chosen.end(), c.second) == chosen.end()) {
            chosen.push_back(c.second);
        }
    }
    if (chosen.empty()) {
        ::MessageBoxW(dlg, L"没有找到带视频的页面。\n请先在受控浏览器里打开视频。", L"自动指定",
                      MB_ICONINFORMATION | MB_OK);
        return;
    }
    AppConfig cfg = ConfigCopy();
    for (size_t i = 0; i < chosen.size() && i < 2; ++i) {
        const PageInfo& p = g_pages[chosen[i]];
        cfg.ch[i].label = i == 0 ? L"视频1" : L"视频2";
        cfg.ch[i].url = Utf8ToWide(p.url);
        cfg.ch[i].title = Utf8ToWide(p.title);
        {
            std::lock_guard<std::mutex> lock(g_app.cfgMutex);
            g_app.cfg.ch[i] = cfg.ch[i];
        }
    }
    g_app.reloadBindings.store(true);
    SaveConfig(cfg);
    UpdateBindInfo(dlg);
    ::SetDlgItemTextW(dlg, IDC_STATE,
                      chosen.size() >= 2
                          ? L"已自动指定两个窗口。"
                          : L"只找到 1 个带视频的页面，另一个请手动指定。");
}

std::wstring FileUrl(const std::wstring& path) {
    std::string utf8 = WideToUtf8(path);
    std::string out = "file:///";
    for (unsigned char c : utf8) {
        if (c == '\\') {
            out.push_back('/');
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out.push_back((char)c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return Utf8ToWide(out);
}

}  // namespace

void LaunchControlledBrowser() {
    AppConfig cfg = ConfigCopy();
    std::wstring exe = cfg.browser;
    if (exe.empty() || ::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exe = FindBrowser();
    }
    if (exe.empty()) {
        ::MessageBoxW(g_bar, L"没有找到 Edge 或 Chrome。", L"启动失败", MB_ICONERROR | MB_OK);
        return;
    }
    ::CreateDirectoryW(cfg.profile.c_str(), nullptr);

    std::wstring cmd = L"\"" + exe + L"\" --remote-debugging-port=" + std::to_wstring(cfg.port) +
                       L" --user-data-dir=\"" + cfg.profile +
                       L"\" --no-first-run --no-default-browser-check "
                       L"--disable-session-crashed-bubble";
    std::wstring help = ExeDir() + L"\\使用说明.html";
    if (::GetFileAttributesW(help.c_str()) != INVALID_FILE_ATTRIBUTES) {
        cmd += L" \"" + FileUrl(help) + L"\"";
    } else {
        cmd += L" about:blank";
    }

    LogLine("启动浏览器: " + WideToUtf8(cmd));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                         &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        LogLine("浏览器已启动");
        return;
    }
    DWORD err = ::GetLastError();
    LogLine("CreateProcess 失败 err=" + std::to_string(err));

    // 兜底：ShellExecute（对 UAC / 路径权限更宽容）
    std::wstring args = cmd.substr(cmd.find(L' ') + 1);
    HINSTANCE h = ::ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr,
                                  SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) > 32) {
        LogLine("ShellExecute 启动成功");
        return;
    }
    std::wstring msg = L"启动浏览器失败（错误代码 " + std::to_wstring(err) +
                       L"）。\n\n可以在开始菜单里手动打开 Edge，加参数：\n--remote-debugging-port=" +
                       std::to_wstring(cfg.port);
    ::MessageBoxW(g_bar, msg.c_str(), L"启动失败", MB_ICONERROR | MB_OK);
}

void OpenSettings() {
    if (g_dialogAlive.load()) {
        if (g_dlg) ::SetForegroundWindow(g_dlg);
        return;
    }
    g_dialogAlive.store(true);
    ::DialogBoxParamW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), g_bar,
                      SettingsProc, 0);
    g_dialogAlive.store(false);
    g_dlg = nullptr;
}

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_dlg = dlg;
            AppConfig cfg = ConfigCopy();
            ::SetDlgItemInt(dlg, IDC_THRESH, (UINT)cfg.thresholdSec, FALSE);
            ::SetDlgItemInt(dlg, IDC_FLASH, (UINT)cfg.flashMs, FALSE);
            ::SetDlgItemInt(dlg, IDC_BEEP, (UINT)cfg.beepSec, FALSE);
            ::SetDlgItemInt(dlg, IDC_FONT, (UINT)cfg.fontPx, FALSE);
            ::SendDlgItemMessageW(dlg, IDC_ALPHA, TBM_SETRANGE, TRUE, MAKELONG(35, 100));
            ::SendDlgItemMessageW(dlg, IDC_ALPHA, TBM_SETPOS, TRUE, cfg.alpha);
            ::CheckDlgButton(dlg, IDC_SOUND, cfg.sound ? BST_CHECKED : BST_UNCHECKED);
            UpdateBindInfo(dlg);
            StartScan(dlg);
            return TRUE;
        }

        case WM_TIMER: {
            ::KillTimer(dlg, 1);
            StartScan(dlg);
            return TRUE;
        }

        case WM_APP_PAGES: {
            auto* r = reinterpret_cast<ScanResult*>(lp);
            if (r) {
                FillList(dlg, *r);
                delete r;
            }
            return TRUE;
        }

        case WM_HSCROLL: {
            int pos = (int)::SendDlgItemMessageW(dlg, IDC_ALPHA, TBM_GETPOS, 0, 0);
            SetBarAlpha(pos);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_LAUNCH:
                    LaunchControlledBrowser();
                    ::SetTimer(dlg, 1, 1800, nullptr);
                    break;
                case IDC_RESCAN:
                    StartScan(dlg);
                    break;
                case IDC_AUTO:
                    AutoBind(dlg);
                    break;
                case IDC_BIND1:
                    BindChannel(dlg, 0);
                    break;
                case IDC_BIND2:
                    BindChannel(dlg, 1);
                    break;
                case IDC_UNBIND: {
                    AppConfig cfg = ConfigCopy();
                    for (int i = 0; i < 2; ++i) {
                        cfg.ch[i].url.clear();
                        cfg.ch[i].title.clear();
                        std::lock_guard<std::mutex> lock(g_app.cfgMutex);
                        g_app.cfg.ch[i] = cfg.ch[i];
                    }
                    g_app.reloadBindings.store(true);
                    SaveConfig(cfg);
                    UpdateBindInfo(dlg);
                    break;
                }
                case IDC_PREVIEW:
                    PlayPreview();
                    break;
                case IDOK: {
                    AppConfig cfg = ConfigCopy();
                    cfg.thresholdSec = (int)::GetDlgItemInt(dlg, IDC_THRESH, nullptr, FALSE);
                    cfg.flashMs = (int)::GetDlgItemInt(dlg, IDC_FLASH, nullptr, FALSE);
                    cfg.beepSec = (int)::GetDlgItemInt(dlg, IDC_BEEP, nullptr, FALSE);
                    cfg.fontPx = (int)::GetDlgItemInt(dlg, IDC_FONT, nullptr, FALSE);
                    cfg.alpha = (int)::SendDlgItemMessageW(dlg, IDC_ALPHA, TBM_GETPOS, 0, 0);
                    cfg.sound = ::IsDlgButtonChecked(dlg, IDC_SOUND) == BST_CHECKED;
                    cfg.thresholdSec = std::max(1, std::min(600, cfg.thresholdSec));
                    cfg.flashMs = std::max(120, std::min(3000, cfg.flashMs));
                    cfg.beepSec = std::max(2, std::min(300, cfg.beepSec));
                    cfg.fontPx = std::max(12, std::min(48, cfg.fontPx));
                    cfg.alpha = std::max(35, std::min(100, cfg.alpha));
                    {
                        std::lock_guard<std::mutex> lock(g_app.cfgMutex);
                        g_app.cfg = cfg;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_app.soundMutex);
                        g_app.soundEnabled = cfg.sound;
                        g_app.beepIntervalSec = cfg.beepSec;
                    }
                    SaveConfig(cfg);
                    ApplyConfigToUi();
                    ::EndDialog(dlg, IDOK);
                    break;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    break;
                default:
                    break;
            }
            return TRUE;

        case WM_CLOSE:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}
