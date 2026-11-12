// 应用级共享结构与全局状态。
#pragma once

#include "cdp.h"  // 先带进 winsock2.h

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#define WM_APP_SNAPSHOT (WM_APP + 1)
#define WM_APP_PAGES (WM_APP + 2)

enum class State { Unbound, Offline, NoVideo, Stopped, Buffering, Playing };

struct ChannelConfig {
    std::wstring label = L"视频";
    std::wstring url;
    std::wstring title;
    bool bound() const { return !url.empty() || !title.empty(); }
};

struct AppConfig {
    int port = 9222;
    std::wstring browser;
    std::wstring profile;
    int thresholdSec = 5;
    int flashMs = 450;
    int beepSec = 8;
    bool sound = true;
    int fontPx = 22;
    int alpha = 100;  // 35..100
    int barX = -99999;
    int barY = -99999;
    ChannelConfig ch[2];
};

struct ChannelSnap {
    State state = State::Unbound;
    bool bound = false;
    double badSec = 0.0;
    double playTime = 0.0;
    double duration = 0.0;
    bool muted = false;
    std::wstring label;
    std::wstring note;
    std::wstring pageTitle;
    std::wstring pageUrl;
    std::wstring targetId;
};

struct Snapshot {
    bool connected = false;
    std::wstring version;
    std::wstring error;
    ChannelSnap ch[2];
    std::vector<PageInfo> pages;
};

struct AppState {
    std::mutex cfgMutex;
    AppConfig cfg;

    std::mutex snapMutex;
    Snapshot snap;

    HWND bar = nullptr;
    std::atomic<bool> acked[2] = {false, false};

    // 界面 → 监控线程
    std::atomic<bool> reloadBindings{true};
    std::atomic<bool> stop{false};

    // 声音线程
    std::mutex soundMutex;
    std::condition_variable soundCv;
    bool alerting = false;
    bool soundEnabled = true;
    int beepIntervalSec = 8;
};

extern AppState g_app;
extern HWND g_bar;

// 配置读写（UTF-8 INI，自己解析，避免 INI API 的编码坑）
AppConfig LoadConfig();
void SaveConfig(const AppConfig& cfg);
std::wstring DefaultProfileDir();
std::wstring FindBrowser();

// 配置快照（加锁复制）
AppConfig ConfigCopy();

// 监控线程
void StartMonitor();
void StopMonitor();

// 声音
void StartSound();
void StopSound();
void SetAlerting(bool on);

// 界面
bool CreateBarWindow(HINSTANCE inst);
void OpenSettings();
void PlayPreview();
void LaunchControlledBrowser();
void ApplyConfigToUi();
void SetBarAlpha(int alphaPercent);
void RefreshBar(HWND bar);
void UpdateBarSize(HWND bar);
void SaveBarPosition(HWND bar);
std::wstring StateText(State s);
const wchar_t* StateIcon(State s);
COLORREF StateColor(State s);
double NowSeconds();
