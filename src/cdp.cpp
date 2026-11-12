#include "cdp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

const char* kProbeJs =
    "(function(){try{"
    "var o='',vs=document.querySelectorAll('video');"
    "for(var i=0;i<vs.length;i++){var v=vs[i],t=0,d=0;"
    "try{t=v.currentTime||0}catch(e){}"
    "try{d=v.duration}catch(e){}"
    "o+=(v.videoWidth|0)+','+(v.videoHeight|0)+','+(v.paused?1:0)+','+(v.ended?1:0)+','+"
    "(v.readyState|0)+','+t.toFixed(2)+','+"
    "((typeof d==='number'&&isFinite(d))?d.toFixed(2):'0')+','+(v.muted?1:0)+';';}"
    "return 'OK|'+o;}catch(e){return 'ERR|'+e;}})()";

unsigned long long nowMs() { return GetTickCount64(); }

// 从 ws://host:port/path 里拆出 host / port / path
bool SplitWsUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    if (url.rfind("ws://", 0) != 0) return false;
    std::string rest = url.substr(5);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    port = colon == std::string::npos ? 80 : std::atoi(hostport.c_str() + colon + 1);
    return !host.empty();
}

void WalkFrames(const json::Value& node, std::vector<std::pair<std::string, std::string>>& out) {
    const json::Value* f = node.get("frame");
    if (f && f->isObj()) {
        const json::Value* id = f->get("id");
        const json::Value* url = f->get("url");
        out.emplace_back(id ? id->s() : "", url ? url->s() : "");
    }
    const json::Value* kids = node.get("childFrames");
    if (kids && kids->isArr()) {
        for (const auto& k : kids->arr) WalkFrames(k, out);
    }
}

// 解析 "OK|w,h,p,e,rs,t,d,m;..." 形式的探针结果
bool ParseProbe(const std::string& text, std::vector<VideoInfo>& out) {
    out.clear();
    if (text.rfind("OK|", 0) != 0) return false;
    size_t pos = 3;
    while (pos < text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) break;
        std::string rec = text.substr(pos, end - pos);
        pos = end + 1;
        VideoInfo v;
        const char* p = rec.c_str();
        char* next = nullptr;
        long vals[6];
        for (int i = 0; i < 6; ++i) {
            vals[i] = std::strtol(p, &next, 10);
            if (next == p) return false;
            p = (*next == ',') ? next + 1 : next;
        }
        v.w = static_cast<int>(vals[0]);
        v.h = static_cast<int>(vals[1]);
        v.paused = static_cast<int>(vals[2]);
        v.ended = static_cast<int>(vals[3]);
        v.readyState = static_cast<int>(vals[4]);
        v.t = std::strtod(p, &next);
        p = (*next == ',') ? next + 1 : next;
        v.d = std::strtod(p, &next);
        p = (*next == ',') ? next + 1 : next;
        v.muted = static_cast<int>(std::strtol(p, nullptr, 10));
        out.push_back(v);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
bool HttpGet(int port, const std::string& path, std::string& body, int timeoutMs) {
    body.clear();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    u_long nonblock = 1;
    ::ioctlsocket(s, FIONBIO, &nonblock);
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
        ::closesocket(s);
        return false;
    }
    if (rc != 0) {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (::select(0, nullptr, &wr, nullptr, &tv) <= 0) {
            ::closesocket(s);
            return false;
        }
    }
    nonblock = 0;
    ::ioctlsocket(s, FIONBIO, &nonblock);

    char req[512];
    std::snprintf(req, sizeof(req),
                  "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nAccept: */*\r\n"
                  "Connection: close\r\n\r\n",
                  path.c_str(), port);
    if (::send(s, req, static_cast<int>(std::strlen(req)), 0) <= 0) {
        ::closesocket(s);
        return false;
    }

    std::string raw;
    unsigned long long deadline = nowMs() + static_cast<unsigned long long>(timeoutMs);
    char tmp[8192];
    while (nowMs() < deadline) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 100 * 1000};
        int sel = ::select(0, &rd, nullptr, nullptr, &tv);
        if (sel == 0) continue;
        if (sel < 0) break;
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        raw.append(tmp, static_cast<size_t>(n));
        if (raw.size() > 8u * 1024u * 1024u) break;
    }
    ::closesocket(s);

    size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) return false;
    if (raw.find(" 200 ") == std::string::npos) return false;
    body = raw.substr(split + 4);
    return true;
}

// ---------------------------------------------------------------------------
// CDP
// ---------------------------------------------------------------------------
bool Cdp::Connect(const std::string& wsUrl, int timeoutMs, std::string* err) {
    Close();
    std::string host, path;
    int port = 0;
    if (!SplitWsUrl(wsUrl, host, port, path)) {
        if (err) *err = "非法 ws 地址";
        return false;
    }
    if (!ws_.connect(host, port, path, timeoutMs, err)) return false;
    alive_ = true;
    id_ = 0;
    return true;
}

void Cdp::Close() {
    ws_.close();
    alive_ = false;
}

bool Cdp::Call(const std::string& method, const std::string& paramsJson, json::Value& result,
               int timeoutMs, std::string* err) {
    if (!alive_) {
        if (err) *err = "连接已关闭";
        return false;
    }
    int myId = ++id_;
    std::string req = "{\"id\":" + std::to_string(myId) + ",\"method\":" + json::quote(method);
    if (!paramsJson.empty()) req += ",\"params\":" + paramsJson;
    req += "}";
    if (!ws_.sendText(req)) {
        alive_ = false;
        if (err) *err = "发送失败";
        return false;
    }

    long long deadline = nowMs() + timeoutMs;
    std::string msg;
    for (;;) {
        long long left = deadline - nowMs();
        if (left <= 0) {
            if (err) *err = method + " 超时";
            return false;
        }
        int r = ws_.recvText(msg, static_cast<int>(left));
        if (r <= 0) {
            alive_ = false;
            if (err) *err = method + " 连接中断";
            return false;
        }
        json::Value doc;
        if (!json::parse(msg, doc)) continue;
        const json::Value* idv = doc.get("id");
        if (!idv || idv->type != json::Type::Num ||
            static_cast<int>(idv->num) != myId) {
            continue;  // 事件或别人的响应，忽略
        }
        const json::Value* e = doc.get("error");
        if (e) {
            const json::Value* m = e->get("message");
            if (err) *err = m ? m->s() : (method + " 失败");
            return false;
        }
        const json::Value* res = doc.get("result");
        result = res ? *res : json::Value();
        return true;
    }
}

bool Cdp::EvalString(const std::string& expr, int contextId, std::string& out, int timeoutMs,
                     std::string* err) {
    std::string params = "{\"expression\":" + json::quote(expr) +
                         ",\"returnByValue\":true,\"awaitPromise\":false";
    if (contextId > 0) params += ",\"contextId\":" + std::to_string(contextId);
    params += "}";
    json::Value res;
    if (!Call("Runtime.evaluate", params, res, timeoutMs, err)) return false;
    if (res.get("exceptionDetails")) {
        if (err) *err = "页面脚本异常";
        return false;
    }
    const json::Value* r = res.get("result");
    const json::Value* v = r ? r->get("value") : nullptr;
    if (!v || !v->isStr()) {
        if (err) *err = "返回值不是字符串";
        return false;
    }
    out = v->str;
    return true;
}

// ---------------------------------------------------------------------------
// 浏览器快照
// ---------------------------------------------------------------------------
BrowserSnapshot FetchBrowser(int port, bool withWindows, int timeoutMs) {
    BrowserSnapshot snap;
    std::string body;
    if (!HttpGet(port, "/json/version", body, timeoutMs)) {
        snap.err = "浏览器没开调试端口";
        return snap;
    }
    json::Value ver;
    if (!json::parse(body, ver)) {
        snap.err = "版本信息解析失败";
        return snap;
    }
    snap.version = ver.get("Browser") ? ver.get("Browser")->s() : "Chromium";

    if (!HttpGet(port, "/json/list", body, timeoutMs)) {
        snap.err = "页面列表读取失败";
        return snap;
    }
    json::Value list;
    if (!json::parse(body, list) || !list.isArr()) {
        snap.err = "页面列表解析失败";
        return snap;
    }
    for (const auto& item : list.arr) {
        if (!item.isObj()) continue;
        const json::Value* type = item.get("type");
        if (!type || type->s() != "page") continue;
        PageInfo p;
        p.id = item.get("id") ? item.get("id")->s() : "";
        p.title = item.get("title") ? item.get("title")->s() : "(无标题)";
        p.url = item.get("url") ? item.get("url")->s() : "";
        p.ws = item.get("webSocketDebuggerUrl") ? item.get("webSocketDebuggerUrl")->s() : "";
        if (!p.id.empty()) snap.pages.push_back(std::move(p));
    }

    if (withWindows && !snap.pages.empty()) {
        std::string wsUrl = ver.get("webSocketDebuggerUrl") ? ver.get("webSocketDebuggerUrl")->s() : "";
        Cdp browser;
        std::string err;
        if (!wsUrl.empty() && browser.Connect(wsUrl, timeoutMs, &err)) {
            for (auto& p : snap.pages) {
                json::Value r;
                std::string params = "{\"targetId\":" + json::quote(p.id) + "}";
                if (browser.Call("Browser.getWindowForTarget", params, r, timeoutMs, &err)) {
                    const json::Value* w = r.get("windowId");
                    if (w) p.windowId = static_cast<int>(w->n(-1));
                }
            }
            browser.Close();
        }
    }
    snap.ok = true;
    return snap;
}

// ---------------------------------------------------------------------------
// FrameProber
// ---------------------------------------------------------------------------
void FrameProber::Reset() {
    frames_.clear();
    lastRefresh_ = 0;
}

bool FrameProber::RefreshFrames(int timeoutMs) {
    json::Value res;
    std::string err;
    if (!cdp_.Call("Page.getFrameTree", "{}", res, timeoutMs, &err)) {
        frames_.clear();
        lastRefresh_ = nowMs();
        return false;
    }
    const json::Value* tree = res.get("frameTree");
    std::vector<std::pair<std::string, std::string>> flat;
    if (tree) WalkFrames(*tree, flat);

    std::vector<Frame> next;
    next.reserve(flat.size());
    for (auto& fp : flat) {
        Frame f;
        f.id = fp.first;
        f.url = fp.second;
        for (auto& old : frames_) {
            if (old.id == f.id) {
                f.ctx = old.ctx;
                break;
            }
        }
        next.push_back(std::move(f));
    }
    frames_ = std::move(next);
    lastRefresh_ = nowMs();
    return !frames_.empty();
}

int FrameProber::ContextOf(Frame& f, int timeoutMs) {
    if (f.ctx > 0) return f.ctx;
    json::Value res;
    std::string err;
    std::string params = "{\"frameId\":" + json::quote(f.id) +
                         ",\"worldName\":\"tvm\",\"grantUniveralAccess\":true}";
    if (!cdp_.Call("Page.createIsolatedWorld", params, res, timeoutMs, &err)) return 0;
    const json::Value* ctx = res.get("executionContextId");
    f.ctx = ctx ? static_cast<int>(ctx->n(0)) : 0;
    return f.ctx;
}

bool FrameProber::Probe(std::vector<VideoInfo>& out, int* frameCount, int timeoutMs) {
    out.clear();
    if (frameCount) *frameCount = 0;
    if (frames_.empty() || nowMs() - lastRefresh_ > 5000) {
        RefreshFrames(timeoutMs);
    }
    if (frames_.empty()) {
        // 兜底：直接在主 frame 求值
        std::string text, err;
        if (!cdp_.EvalString(kProbeJs, 0, text, timeoutMs, &err)) return false;
        return ParseProbe(text, out);
    }
    if (frameCount) *frameCount = static_cast<int>(frames_.size());

    int budget = 12;
    for (size_t i = 0; i < frames_.size(); ++i) {
        Frame& f = frames_[i];
        // about:blank 基本都是填充广告位，跳过省资源（主 frame 永远探）
        if (i > 0 && (f.url.empty() || f.url == "about:blank")) continue;
        if (budget-- <= 0) break;
        int ctx = ContextOf(f, timeoutMs);
        if (ctx <= 0) continue;
        std::string text, err;
        if (!cdp_.EvalString(kProbeJs, ctx, text, timeoutMs, &err)) {
            f.ctx = 0;  // 上下文失效（导航/重建），下轮重试
            continue;
        }
        std::vector<VideoInfo> part;
        if (!ParseProbe(text, part)) continue;
        for (size_t v = 0; v < part.size(); ++v) {
            part[v].key = std::to_string(i) + ":" + std::to_string(v);
            out.push_back(part[v]);
        }
    }
    return true;
}

int CountVideos(FrameProber& prober, int timeoutMs) {
    std::vector<VideoInfo> vids;
    int frames = 0;
    if (!prober.Probe(vids, &frames, timeoutMs)) return -1;
    int n = 0;
    for (const auto& v : vids) {
        if (v.w > 0 || v.h > 0 || v.readyState >= 2 || v.d > 0) ++n;
    }
    return n;
}
