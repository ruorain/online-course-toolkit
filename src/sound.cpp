#include "app.h"

#include "util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "app.h"

#include "util.h"

namespace {

std::thread g_thread;
std::atomic<bool> g_running{false};

bool BeepOnce(int freq, int ms) {
    if (::Beep(freq, ms)) return true;
    // 有些机器/独占音频设备上 Beep 会失败，退回系统提示音
    ::MessageBeep(MB_ICONHAND);
    ::Sleep(ms);
    return false;
}

void Pattern() {
    static bool logged = false;
    bool ok = BeepOnce(1050, 130);
    if (!logged) {
        logged = true;
        LogLine(std::string("提示音 Beep ") + (ok ? "正常" : "失败，已退回 MessageBeep") +
                " err=" + std::to_string(::GetLastError()));
    }
    ::Sleep(55);
    BeepOnce(1450, 130);
    ::Sleep(55);
    BeepOnce(1050, 130);
    ::Sleep(55);
    BeepOnce(1450, 230);
}

}  // namespace

void PlayPreview() { std::thread([] { Pattern(); }).detach(); }

void SetAlerting(bool on) {
    {
        std::lock_guard<std::mutex> lock(g_app.soundMutex);
        g_app.alerting = on;
    }
    g_app.soundCv.notify_all();
}

void StartSound() {
    if (g_running.exchange(true)) return;
    g_thread = std::thread([] {
        for (;;) {
            std::unique_lock<std::mutex> lock(g_app.soundMutex);
            g_app.soundCv.wait(lock, [] {
                return !g_running.load() || (g_app.alerting && g_app.soundEnabled);
            });
            if (!g_running.load()) break;
            lock.unlock();
            Pattern();
            lock.lock();
            int interval = std::max(2, g_app.beepIntervalSec);
            g_app.soundCv.wait_for(lock, std::chrono::seconds(interval), [] {
                return !g_running.load() || !g_app.alerting || !g_app.soundEnabled;
            });
            if (!g_running.load()) break;
        }
    });
}

void StopSound() {
    if (!g_running.exchange(false)) return;
    g_app.soundCv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}
