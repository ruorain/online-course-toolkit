// 命令行自检工具：打印每个标签页里有几个视频（穿透 iframe）。
// 用法：probe.exe [端口]
#include "../src/cdp.h"  // 先带进 winsock2.h
#include "../src/util.h"

#include <windows.h>

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup 失败\n");
        return 1;
    }
    int port = argc > 1 ? std::atoi(argv[1]) : 9222;
    BrowserSnapshot snap = FetchBrowser(port, true, 2000);
    if (!snap.ok) {
        std::printf("连接失败(端口 %d): %s\n", port, snap.err.c_str());
        return 1;
    }
    std::printf("%s · %zu 个页面\n", snap.version.c_str(), snap.pages.size());
    for (auto& p : snap.pages) {
        Cdp cdp;
        std::string err;
        if (!cdp.Connect(p.ws, 2500, &err)) {
            std::printf("  [连接失败] %s (%s)\n", p.title.c_str(), err.c_str());
            continue;
        }
        FrameProber prober(cdp);
        std::vector<VideoInfo> vids;
        int frames = 0;
        bool ok = prober.Probe(vids, &frames, 1800);
        std::printf("  %-28s frame=%-3d 视频=%zu%s\n", p.title.c_str(), frames, vids.size(),
                    ok ? "" : "  [探测失败]");
        for (auto& v : vids) {
            std::printf("      %-5s %4dx%-4d %s rs=%d t=%7.2f/%.2f\n", v.key.c_str(), v.w, v.h,
                        v.paused ? "暂停" : "播放", v.readyState, v.t, v.d);
        }
        cdp.Close();
    }
    ::WSACleanup();
    return 0;
}
