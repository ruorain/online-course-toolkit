#include "app.h"
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include <windowsx.h>
#include "util.h"

HWND g_bar = nullptr;  // 设置窗口也要用

namespace {

const COLORREF kBg = RGB(27, 29, 35);
const COLORREF kBorder = RGB(59, 65, 80);
const COLORREF kSep = RGB(86, 93, 109);
const COLORREF kPlay = RGB(46, 229, 157);
const COLORREF kStop = RGB(255, 93, 93);
const COLORREF kWarn = RGB(255, 176, 32);
const COLORREF kOff = RGB(255, 138, 128);
const COLORREF kIdle = RGB(138, 144, 160);
const COLORREF kFlashA = RGB(255, 45, 85);
const COLORREF kFlashB = RGB(255, 225, 77);
const COLORREF kAckBg = RGB(255, 209, 102);
const COLORREF kAckFg = RGB(27, 29, 35);
const COLORREF kAckedBg = RGB(74, 26, 32);
const COLORREF kAckedFg = RGB(255, 155, 155);
const COLORREF kPickBg = RGB(58, 91, 217);
const COLORREF kPickFg = RGB(255, 255, 255);

const wchar_t* kBarClass = L"TvmBarWnd";

enum {
    IDM_SETTINGS = 100,
    IDM_SOUND,
    IDM_TOPMOST,
    IDM_PREVIEW,
    IDM_LAUNCH,
    IDM_QUIT,
};

HFONT g_font = nullptr;
HFONT g_fontSmall = nullptr;
UINT g_dpi = 96;

Snapshot g_ui;
bool g_haveSnap = false;
bool g_alerting = false;
bool g_flashOn = false;
bool g_acked[2] = {false, false};
bool g_topmost = true;

std::wstring g_text[2];
RECT g_seg[2]{};
RECT g_ackRect{};
RECT g_pickRect{};
RECT g_sepRect{};
int g_w = 0, g_h = 0;
bool g_dragging = false;
POINT g_dragOffset{};
bool g_showAck = false;
bool g_showPick = false;

bool IsBad(State s) {
    return s == State::Stopped || s == State::Buffering || s == State::NoVideo ||
           s == State::Offline;
}

bool Alerting(int i) {
    if (!g_haveSnap) return false;
    const ChannelSnap& c = g_ui.ch[i];
    if (!IsBad(c.state)) return false;
    double threshold = (double)ConfigCopy().thresholdSec;
    return c.badSec >= threshold && !g_acked[i];
}

std::wstring FmtTime(double sec) {
    int total = (int)(sec < 0 ? 0 : sec);
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    wchar_t buf[32];
    if (h > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    } else {
        swprintf(buf, 32, L"%02d:%02d", m, s);
    }
    return buf;
}

std::wstring BuildText(int i) {
    const ChannelSnap& c = g_ui.ch[i];
    std::wstring label = c.label.empty() ? (i == 0 ? L"视频1" : L"视频2") : c.label;
    if (c.state == State::Unbound) return L"… " + label + L" 未指定";

    std::wstring extra;
    if (c.state == State::Playing) {
        extra = L" " + FmtTime(c.playTime);
    } else if (c.state == State::Stopped) {
        if (c.badSec >= (double)ConfigCopy().thresholdSec) {
            extra = L" " + std::to_wstring((int)c.badSec) + L"s";
        } else if (c.note.find(L"结束") != std::wstring::npos) {
            extra = L"（播放结束）";
        }
    } else if (c.state == State::Buffering || c.state == State::NoVideo ||
               c.state == State::Offline) {
        if (c.badSec >= (double)ConfigCopy().thresholdSec) {
            extra = L" " + std::to_wstring((int)c.badSec) + L"s";
        }
    }
    if (g_acked[i] && IsBad(c.state)) extra += L" · 已静音";
    return StateIcon(c.state) + std::wstring(L" ") + label + L" " + StateText(c.state) + extra;
}

void MakeFonts(const AppConfig& cfg) {
    if (g_font) ::DeleteObject(g_font);
    if (g_fontSmall) ::DeleteObject(g_fontSmall);
    int px = cfg.fontPx;
    g_font = ::CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_fontSmall = ::CreateFontW(-(px * 3 / 4), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                L"Microsoft YaHei UI");
}

int Measure(HDC dc, const std::wstring& text) {
    SIZE sz{};
    HGDIOBJ old = ::SelectObject(dc, g_font);
    ::GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    ::SelectObject(dc, old);
    return sz.cx;
}

void LayoutBar(HWND hWnd, bool allowResize) {
    HDC dc = ::GetDC(hWnd);
    int padX = std::max(8, 10 * (int)g_dpi / 96);
    int padY = std::max(2, 3 * (int)g_dpi / 96);

    int textH = 0;
    {
        HGDIOBJ old = ::SelectObject(dc, g_font);
        TEXTMETRICW tm{};
        ::GetTextMetricsW(dc, &tm);
        textH = tm.tmHeight;
        ::SelectObject(dc, old);
    }
    g_h = textH + padY * 2 + 2;

    int x = 1;
    for (int i = 0; i < 2; ++i) {
        g_text[i] = BuildText(i);
        int w = Measure(dc, g_text[i]) + padX * 2;
        g_seg[i] = RECT{x, 1, x + w, g_h - 1};
        x += w;
        if (i == 0) {
            int sw = Measure(dc, L"│") + 6;
            g_sepRect = RECT{x, 1, x + sw, g_h - 1};
            x += sw;
        }
    }

    g_showAck = Alerting(0) || Alerting(1);
    g_showPick = (g_ui.ch[0].state == State::Unbound) || (g_ui.ch[1].state == State::Unbound) ||
                 !g_ui.connected;
    g_ackRect = RECT{};
    g_pickRect = RECT{};
    if (g_showAck) {
        int w = Measure(dc, L"我知道了") + padX * 2;
        g_ackRect = RECT{x + 2, 1, x + 2 + w, g_h - 1};
        x += w + 4;
    }
    if (g_showPick) {
        int w = Measure(dc, L"⚙ 选择窗口") + padX * 2;
        g_pickRect = RECT{x + 2, 1, x + 2 + w, g_h - 1};
        x += w + 4;
    }
    g_w = x + 2;
    ::ReleaseDC(hWnd, dc);

    if (allowResize) {
        RECT rc;
        ::GetWindowRect(hWnd, &rc);
        if (rc.right - rc.left != g_w || rc.bottom - rc.top != g_h) {
            ::SetWindowPos(hWnd, HWND_TOPMOST, rc.left, rc.top, g_w, g_h,
                           SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
    }
    ::InvalidateRect(hWnd, nullptr, FALSE);
}

void PaintBar(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC dc = ::BeginPaint(hWnd, &ps);
    RECT client;
    ::GetClientRect(hWnd, &client);
    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBmp = ::SelectObject(mem, bmp);

    HBRUSH bg = ::CreateSolidBrush(kBg);
    ::FillRect(mem, &client, bg);
    ::DeleteObject(bg);

    // 边框：报警时整条跟着闪
    HBRUSH border = ::CreateSolidBrush(g_alerting ? (g_flashOn ? kFlashA : kFlashB) : kBorder);
    RECT borderRc = client;
    ::FrameRect(mem, &borderRc, border);
    ::DeleteObject(border);

    ::SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = ::SelectObject(mem, g_font);

    for (int i = 0; i < 2; ++i) {
        const ChannelSnap& c = g_ui.ch[i];
        bool alert = Alerting(i);
        COLORREF fg = StateColor(c.state);
        COLORREF bgr = kBg;
        if (alert) {
            bgr = g_flashOn ? kFlashA : kFlashB;
            fg = g_flashOn ? RGB(255, 255, 255) : RGB(0, 0, 0);
        } else if (g_acked[i] && IsBad(c.state)) {
            bgr = kAckedBg;
            fg = kAckedFg;
        }
        HBRUSH b = ::CreateSolidBrush(bgr);
        ::FillRect(mem, &g_seg[i], b);
        ::DeleteObject(b);
        ::SetTextColor(mem, fg);
        RECT r = g_seg[i];
        ::DrawTextW(mem, g_text[i].c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 分隔符
    {
        HBRUSH b = ::CreateSolidBrush(kBg);
        ::FillRect(mem, &g_sepRect, b);
        ::DeleteObject(b);
        ::SetTextColor(mem, kSep);
        RECT r = g_sepRect;
        ::DrawTextW(mem, L"│", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (g_showAck) {
        HBRUSH b = ::CreateSolidBrush(kAckBg);
        ::FillRect(mem, &g_ackRect, b);
        ::DeleteObject(b);
        ::SetTextColor(mem, kAckFg);
        RECT r = g_ackRect;
        ::DrawTextW(mem, L"我知道了", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (g_showPick) {
        HBRUSH b = ::CreateSolidBrush(kPickBg);
        ::FillRect(mem, &g_pickRect, b);
        ::DeleteObject(b);
        ::SetTextColor(mem, kPickFg);
        RECT r = g_pickRect;
        ::DrawTextW(mem, L"⚙ 选择窗口", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    ::SelectObject(mem, oldFont);
    ::BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hWnd, &ps);
}

bool PointIn(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

void Acknowledge() {
    for (int i = 0; i < 2; ++i) {
        if (Alerting(i)) g_acked[i] = true;
    }
    SetAlerting(false);
    LayoutBar(g_bar, false);
}

void ShowMenu(HWND hWnd, POINT pt) {
    if (g_dragging) {
        g_dragging = false;
        ::ReleaseCapture();
    }
    AppConfig cfg = ConfigCopy();
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"① 选择要监控的两个窗口…");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (cfg.sound ? MF_CHECKED : 0), IDM_SOUND, L"提示音");
    ::AppendMenuW(menu, MF_STRING | (g_topmost ? MF_CHECKED : 0), IDM_TOPMOST, L"始终置顶");
    ::AppendMenuW(menu, MF_STRING, IDM_PREVIEW, L"试听提示音");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_LAUNCH, L"打开受控浏览器");
    ::AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出");

    RECT rc;
    ::GetWindowRect(hWnd, &rc);
    ::SetForegroundWindow(hWnd);  // 不先抢前台，菜单会出现但点不动
    UINT flags = TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON;
    int cmd = (int)::TrackPopupMenu(menu, flags, pt.x, rc.bottom + 2, 0, hWnd, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hWnd, WM_NULL, 0, 0);  // 释放菜单的鼠标捕获

    switch (cmd) {
        case IDM_SETTINGS:
            OpenSettings();
            break;
        case IDM_SOUND: {
            AppConfig c = ConfigCopy();
            c.sound = !c.sound;
            {
                std::lock_guard<std::mutex> lock(g_app.cfgMutex);
                g_app.cfg.sound = c.sound;
            }
            {
                std::lock_guard<std::mutex> lock(g_app.soundMutex);
                g_app.soundEnabled = c.sound;
            }
            SaveConfig(c);
            SetAlerting(!c.sound ? false : g_alerting);
            break;
        }
        case IDM_TOPMOST:
            g_topmost = !g_topmost;
            ::SetWindowPos(hWnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case IDM_PREVIEW:
            PlayPreview();
            break;
        case IDM_LAUNCH:
            LaunchControlledBrowser();
            break;
        case IDM_QUIT:
            ::PostMessageW(hWnd, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK BarProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_bar = hWnd;
            ::SetTimer(hWnd, 1, 450, nullptr);
            ::SetTimer(hWnd, 2, 2000, nullptr);
            return 0;

        case WM_APP_SNAPSHOT: {
            {
                std::lock_guard<std::mutex> lock(g_app.snapMutex);
                g_ui = g_app.snap;
                g_haveSnap = true;
            }
            for (int i = 0; i < 2; ++i) {
                if (!IsBad(g_ui.ch[i].state)) g_acked[i] = false;
            }
            LayoutBar(hWnd, true);
            return 0;
        }

        case WM_TIMER:
            if (wp == 1) {
                bool any = Alerting(0) || Alerting(1);
                if (any) {
                    g_flashOn = !g_flashOn;
                    ::InvalidateRect(hWnd, nullptr, FALSE);
                } else if (g_alerting) {
                    ::InvalidateRect(hWnd, nullptr, FALSE);
                }
                if (any != g_alerting) {
                    g_alerting = any;
                    SetAlerting(any);
                    ::InvalidateRect(hWnd, nullptr, FALSE);
                }
                AppConfig cfg = ConfigCopy();
                ::SetTimer(hWnd, 1, (UINT)cfg.flashMs, nullptr);
            } else if (wp == 2 && g_topmost) {
                ::SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return 0;

        case WM_PAINT:
            PaintBar(hWnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_showPick && PointIn(g_pickRect, p)) {
                OpenSettings();
                return 0;
            }
            if (g_showAck && PointIn(g_ackRect, p)) {
                Acknowledge();
                return 0;
            }
            RECT rc;
            ::GetWindowRect(hWnd, &rc);
            g_dragOffset = POINT{p.x, p.y};  // 相对窗口的偏移，不是屏幕坐标
            g_dragging = true;
            ::SetCapture(hWnd);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_dragging) {
                POINT cur;
                ::GetCursorPos(&cur);
                ::SetWindowPos(hWnd, HWND_TOPMOST, cur.x - g_dragOffset.x,
                               cur.y - g_dragOffset.y, 0, 0,
                               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
            }
            return 0;

        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = false;
                ::ReleaseCapture();
                SaveBarPosition(hWnd);
                ::InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            g_dragging = false;
            return 0;

        case WM_LBUTTONDBLCLK:
            OpenSettings();
            return 0;

        case WM_RBUTTONUP: {
            POINT pt;
            ::GetCursorPos(&pt);
            ShowMenu(hWnd, pt);
            return 0;
        }

        case WM_CLOSE:
            StopMonitor();
            StopSound();
            ::DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hWnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
std::wstring StateText(State s) {
    switch (s) {
        case State::Playing: return L"播放中";
        case State::Stopped: return L"已停止";
        case State::Buffering: return L"卡住了";
        case State::NoVideo: return L"无视频";
        case State::Offline: return L"未连接";
        default: return L"未指定";
    }
}

const wchar_t* StateIcon(State s) {
    switch (s) {
        case State::Playing: return L"●";
        case State::Stopped: return L"▲";
        case State::Buffering: return L"◐";
        case State::NoVideo: return L"○";
        case State::Offline: return L"✕";
        default: return L"…";
    }
}

COLORREF StateColor(State s) {
    switch (s) {
        case State::Playing: return kPlay;
        case State::Stopped: return kStop;
        case State::Buffering: return kWarn;
        case State::NoVideo: return kWarn;
        case State::Offline: return kOff;
        default: return kIdle;
    }
}

static void ClampToScreen(int& x, int& y, int w, int h) {
    int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = 1920; vh = 1080; }
    if (x < vx || x > vx + vw - 120 || y < vy || y > vy + vh - 20) {
        x = vx + (vw - w) / 2;
        y = vy + 40;
    } else {
        if (x + w > vx + vw) x = vx + vw - w;
        if (y + h > vy + vh) y = vy + vh - h;
    }
}

void SaveBarPosition(HWND hWnd) {
    RECT rc;
    ::GetWindowRect(hWnd, &rc);
    int px = rc.left, py = rc.top;
    ClampToScreen(px, py, rc.right - rc.left, rc.bottom - rc.top);
    rc.left = px;
    rc.top = py;
    ::SetWindowPos(hWnd, HWND_TOPMOST, rc.left, rc.top, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
    AppConfig cfg = ConfigCopy();
    cfg.barX = rc.left;
    cfg.barY = rc.top;
    {
        std::lock_guard<std::mutex> lock(g_app.cfgMutex);
        g_app.cfg.barX = cfg.barX;
        g_app.cfg.barY = cfg.barY;
    }
    SaveConfig(cfg);
}

void RefreshBar(HWND hWnd) {
    if (!hWnd) return;
    {
        std::lock_guard<std::mutex> lock(g_app.snapMutex);
        g_ui = g_app.snap;
        g_haveSnap = true;
    }
    LayoutBar(hWnd, true);
}

void UpdateBarSize(HWND hWnd) { LayoutBar(hWnd, true); }

void SetBarAlpha(int alphaPercent) {
    if (!g_bar) return;
    LONG_PTR ex = ::GetWindowLongPtrW(g_bar, GWL_EXSTYLE);
    if (alphaPercent >= 100) {
        ::SetWindowLongPtrW(g_bar, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        ::RedrawWindow(g_bar, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    } else {
        ::SetWindowLongPtrW(g_bar, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        ::SetLayeredWindowAttributes(g_bar, 0, (BYTE)(alphaPercent * 255 / 100), LWA_ALPHA);
    }
}

void ApplyConfigToUi() {
    AppConfig cfg = ConfigCopy();
    MakeFonts(cfg);
    g_topmost = true;
    if (g_bar) {
        SetBarAlpha(cfg.alpha);
        ::SetTimer(g_bar, 1, (UINT)cfg.flashMs, nullptr);
        LayoutBar(g_bar, true);
    }
}

bool CreateBarWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BarProc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = kBarClass;
    wc.hIcon = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16,
                                     LR_DEFAULTCOLOR);
    if (!::RegisterClassExW(&wc)) return false;

    AppConfig cfg = ConfigCopy();
    g_dpi = 96;
    HDC dc = ::GetDC(nullptr);
    if (dc) {
        g_dpi = (UINT)::GetDeviceCaps(dc, LOGPIXELSX);
        ::ReleaseDC(nullptr, dc);
    }
    MakeFonts(cfg);

    int x = cfg.barX, y = cfg.barY;
    ClampToScreen(x, y, 420, 40);
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (cfg.alpha < 100) exStyle |= WS_EX_LAYERED;
    g_bar = ::CreateWindowExW(exStyle, kBarClass, L"视频播放监控", WS_POPUP, x, y, 400, 36,
                              nullptr, nullptr, inst, nullptr);
    if (!g_bar) return false;
    if (cfg.alpha < 100) {
        ::SetLayeredWindowAttributes(g_bar, 0, (BYTE)(cfg.alpha * 255 / 100), LWA_ALPHA);
    }
    g_app.bar = g_bar;
    ::ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    RefreshBar(g_bar);
    return true;
}
