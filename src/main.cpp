#include "app.h"  // 先带进 winsock2.h / windows.h

#include <commctrl.h>

#include <mutex>
#include <string>

#include "resource.h"
#include "util.h"

namespace {

HANDLE g_mutex = nullptr;

void Fatal(const wchar_t* text) {
    ::MessageBoxW(nullptr, text, L"视频播放监控条", MB_ICONERROR | MB_OK);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // manifest 已经声明 DPI 感知；这里再兜底一次（老系统/被覆盖时）
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto setCtx = reinterpret_cast<SetCtxFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx) setCtx(reinterpret_cast<HANDLE>(-4));  // PER_MONITOR_AWARE_V2
    }
    ::InitCommonControls();

    LogLine("启动");

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Fatal(L"网络初始化失败。");
        return 1;
    }

    g_mutex = ::CreateMutexW(nullptr, TRUE, L"Local\\TopVideoMonitor_Mutex");
    if (g_mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(nullptr, L"视频播放监控条已经在运行了。\n\n右键顶部那条状态栏可以打开设置。",
                      L"视频播放监控条", MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_app.cfgMutex);
        g_app.cfg = LoadConfig();
        g_app.soundEnabled = g_app.cfg.sound;
        g_app.beepIntervalSec = g_app.cfg.beepSec;
    }

    if (!CreateBarWindow(inst)) {
        Fatal(L"创建状态条窗口失败。");
        return 1;
    }

    LogLine("窗口已创建，启动监控线程");
    StartSound();
    StartMonitor();

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    StopMonitor();
    StopSound();
    ::WSACleanup();
    return 0;
}
