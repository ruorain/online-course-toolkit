// 零依赖 CDP 客户端：HTTP 端点 + WebSocket JSON-RPC + 逐 frame 探测视频。
#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "ws.h"

struct PageInfo {
    std::string id;
    std::string title;
    std::string url;
    std::string ws;
    int windowId = -1;
    int videos = -1;  // -1 表示还没统计
};

struct BrowserSnapshot {
    bool ok = false;
    std::string version;
    std::string err;
    std::vector<PageInfo> pages;
};

// GET http://127.0.0.1:<port><path>，返回 body
bool HttpGet(int port, const std::string& path, std::string& body, int timeoutMs);

// 一次拿全：浏览器版本 + 页面列表（可选按窗口分组）
BrowserSnapshot FetchBrowser(int port, bool withWindows, int timeoutMs);

class Cdp {
public:
    bool Connect(const std::string& wsUrl, int timeoutMs, std::string* err);
    void Close();
    bool Alive() const { return alive_; }

    bool Call(const std::string& method, const std::string& paramsJson, json::Value& result,
              int timeoutMs, std::string* err);
    bool EvalString(const std::string& expr, int contextId, std::string& out, int timeoutMs,
                    std::string* err);

private:
    WsClient ws_;
    int id_ = 0;
    bool alive_ = false;
};

struct VideoInfo {
    int w = 0;
    int h = 0;
    int paused = 1;
    int ended = 0;
    int readyState = 0;
    int muted = 0;
    double t = 0.0;
    double d = 0.0;
    std::string key;  // "frameIndex:videoIndex"，用来跨轮次比对 currentTime
};

// 穿透 iframe：按 frame tree 在每层 frame 的隔离环境里读 <video>
class FrameProber {
public:
    explicit FrameProber(Cdp& cdp) : cdp_(cdp) {}
    void Reset();
    bool Probe(std::vector<VideoInfo>& out, int* frameCount, int timeoutMs);

private:
    struct Frame {
        std::string id;
        std::string url;
        int ctx = 0;
    };

    bool RefreshFrames(int timeoutMs);
    int ContextOf(Frame& f, int timeoutMs);

    Cdp& cdp_;
    std::vector<Frame> frames_;
    unsigned long long lastRefresh_ = 0;
};

// 只统计有画面的 video 个数（给窗口列表用）
int CountVideos(FrameProber& prober, int timeoutMs);
