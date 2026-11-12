#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <thread>

#include "app.h"
#include "util.h"

namespace {

struct ChannelRuntime {
    ChannelRuntime() : prober(cdp) {}

    Cdp cdp;
    FrameProber prober;
    std::string targetId;
    std::string pageWs;
    State state = State::Unbound;
    std::wstring note;
    std::wstring pageTitle;
    std::wstring pageUrl;
    double badSince = -1.0;
    double playTime = 0.0;
    double duration = 0.0;
    bool muted = false;
    std::map<std::string, double> lastT;
};

ChannelRuntime g_ch[2];

void ResetRuntime(ChannelRuntime& ch) {
    ch.cdp.Close();
    ch.prober.Reset();
    ch.targetId.clear();
    ch.pageWs.clear();
    ch.state = State::Unbound;
    ch.note.clear();
    ch.badSince = -1.0;
    ch.playTime = 0.0;
    ch.duration = 0.0;
    ch.lastT.clear();
}

std::wstring NormalizeUrl(const std::wstring& url) {
    size_t hash = url.find(L'#');
    return hash == std::wstring::npos ? url : url.substr(0, hash);
}

int ScorePage(const PageInfo& p, const ChannelConfig& cfg) {
    std::wstring purl = Utf8ToWide(p.url);
    std::wstring ptitle = Utf8ToWide(p.title);
    if (!cfg.url.empty()) {
        if (purl == cfg.url) return 5;
        if (NormalizeUrl(purl) == NormalizeUrl(cfg.url)) return 4;
    }
    if (!cfg.title.empty()) {
        if (ptitle == cfg.title) return 3;
        if (cfg.title.size() >= 6 && ptitle.find(cfg.title.substr(0, 12)) != std::wstring::npos) {
            return 2;
        }
    }
    return 0;
}

void SetOffline(ChannelRuntime& ch, const std::wstring& note) {
    ch.state = State::Offline;
    ch.note = note;
    ch.lastT.clear();
    ch.playTime = 0.0;
}

void ApplyState(ChannelRuntime& ch, State s, const std::wstring& note, double now) {
    if (s != ch.state) ch.state = s;
    ch.note = note;
    bool bad = (s == State::Stopped || s == State::Buffering || s == State::NoVideo ||
                s == State::Offline);
    if (bad) {
        if (ch.badSince < 0) ch.badSince = now;
    } else {
        ch.badSince = -1.0;
    }
    if (s == State::Unbound) ch.badSince = -1.0;
}

void UpdateFromProbe(ChannelRuntime& ch, const std::vector<VideoInfo>& vids, double now) {
    std::map<std::string, double> prev = ch.lastT;
    ch.lastT.clear();
    for (const auto& v : vids) ch.lastT[v.key] = v.t;

    std::vector<const VideoInfo*> cand;
    for (const auto& v : vids) {
        if (v.w > 0 || v.h > 0 || v.readyState >= 2 || v.d > 0) cand.push_back(&v);
    }
    if (cand.empty()) {
        ch.playTime = 0.0;
        ApplyState(ch, State::NoVideo, L"页面里没有视频", now);
        return;
    }

    const VideoInfo* playing = nullptr;
    bool stalled = false, ended = false;
    for (const VideoInfo* v : cand) {
        if (v->ended) {
            ended = true;
            continue;
        }
        if (v->paused) continue;
        if (v->readyState < 2) {
            stalled = true;
            continue;
        }
        auto it = prev.find(v->key);
        if (it == prev.end()) {
            playing = v;
            break;
        }
        double delta = v->t - it->second;
        bool looped = v->d > 0 && it->second > v->d - 1.5 && v->t < 1.5;
        if ((delta > 0.02 && delta < 3.0) || looped) {
            playing = v;
            break;
        }
        stalled = true;
    }

    if (playing) {
        ch.playTime = playing->t;
        ch.duration = playing->d;
        ch.muted = playing->muted != 0;
        ApplyState(ch, State::Playing, L"", now);
    } else if (stalled) {
        ApplyState(ch, State::Buffering, L"时间不走（缓冲/卡住）", now);
    } else if (ended) {
        ApplyState(ch, State::Stopped, L"播放结束", now);
    } else {
        ApplyState(ch, State::Stopped, L"已暂停", now);
    }
}

void Publish() {
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(g_app.snapMutex);
        Snapshot& s = g_app.snap;
        s.ch[0] = ChannelSnap();
        s.ch[1] = ChannelSnap();
        for (int i = 0; i < 2; ++i) {
            const ChannelRuntime& ch = g_ch[i];
            ChannelSnap& d = s.ch[i];
            d.state = ch.state;
            d.bound = !ch.targetId.empty();
            d.badSec = ch.badSince >= 0 ? std::max(0.0, NowSeconds() - ch.badSince) : 0.0;
            d.playTime = ch.playTime;
            d.duration = ch.duration;
            d.muted = ch.muted;
            d.note = ch.note;
            d.pageTitle = ch.pageTitle;
            d.pageUrl = ch.pageUrl;
            d.targetId = Utf8ToWide(ch.targetId);
        }
        snap = s;
    }
    if (g_app.bar) ::PostMessageW(g_app.bar, WM_APP_SNAPSHOT, 0, 0);
}

void Tick() {
    AppConfig cfg = ConfigCopy();
    double now = NowSeconds();

    BrowserSnapshot browser = FetchBrowser(cfg.port, false, 900);

    {
        std::lock_guard<std::mutex> lock(g_app.snapMutex);
        g_app.snap.connected = browser.ok;
        g_app.snap.version = Utf8ToWide(browser.version);
        g_app.snap.error = Utf8ToWide(browser.err);
        g_app.snap.pages = browser.pages;
    }

    if (g_app.reloadBindings.exchange(false)) {
        for (auto& ch : g_ch) ResetRuntime(ch);
    }

    for (int i = 0; i < 2; ++i) {
        ChannelRuntime& ch = g_ch[i];
        const ChannelConfig& cc = cfg.ch[i];

        if (!cc.bound()) {
            if (ch.state != State::Unbound) {
                ResetRuntime(ch);
                ch.state = State::Unbound;
            }
            continue;
        }

        if (!browser.ok) {
            SetOffline(ch, L"浏览器未连接");
            continue;
        }

        // 已经绑定的 target 只要还在就继续用（换章节、URL 变化都不影响）
        const PageInfo* page = nullptr;
        if (!ch.targetId.empty()) {
            for (const auto& p : browser.pages) {
                if (p.id == ch.targetId) {
                    page = &p;
                    break;
                }
            }
            if (!page) {
                ch.targetId.clear();
                ch.cdp.Close();
                ch.prober.Reset();
            }
        }
        if (!page) {
            int best = 0;
            for (const auto& p : browser.pages) {
                int score = ScorePage(p, cc);
                if (score > best) {
                    best = score;
                    page = &p;
                }
            }
            if (page && best >= 2) {
                ch.targetId = page->id;
            } else {
                page = nullptr;
            }
        }
        if (!page) {
            SetOffline(ch, L"找不到对应窗口，请重新指定");
            continue;
        }
        ch.pageTitle = Utf8ToWide(page->title);
        ch.pageUrl = Utf8ToWide(page->url);

        if (page->ws.empty()) {
            SetOffline(ch, L"无法连接该标签页");
            continue;
        }
        if (!ch.cdp.Alive() || ch.pageWs != page->ws) {
            std::string err;
            ch.cdp.Close();
            ch.prober.Reset();
            if (!ch.cdp.Connect(page->ws, 1500, &err)) {
                SetOffline(ch, L"读取页面失败，正在重连");
                continue;
            }
            ch.pageWs = page->ws;
        }

        std::vector<VideoInfo> vids;
        std::string err;
        if (!ch.prober.Probe(vids, nullptr, 1200)) {
            ch.cdp.Close();
            ch.prober.Reset();
            SetOffline(ch, L"读取页面失败，正在重连");
            continue;
        }
        UpdateFromProbe(ch, vids, now);
    }

    Publish();
}

}  // namespace

void StartMonitor() {
    std::thread([] {
        // 让 UI 先建好窗口
        ::Sleep(200);
        while (!g_app.stop.load()) {
            try {
                Tick();
            } catch (...) {
                LogLine("monitor tick 异常");
            }
            ::Sleep(600);
        }
    }).detach();
}

void StopMonitor() { g_app.stop.store(true); }
