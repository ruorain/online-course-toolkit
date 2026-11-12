# -*- coding: utf-8 -*-
"""开发期：在真实站点上验证"穿透 iframe 的播放判定"（暂停/恢复）。

    python devcheck.py [url关键词] [端口]
"""
import sys
import time

import cdp
from frames import FrameProber

PAUSE = "document.querySelectorAll('video').forEach(function(v){v.pause();}); 'ok'"
PLAY = "document.querySelectorAll('video').forEach(function(v){v.play();}); 'ok'"


def show(prober, tag):
    res = prober.probe(1.5)
    vids = res.get("vs") or []
    print("%-6s frames=%s 视频=%d %s" % (
        tag, res.get("frames"), len(vids),
        [(v.get("k"), v.get("w"), v.get("h"), "暂停" if v.get("p") else "播放",
          round(v.get("t") or 0, 2)) for v in vids]))


def act(prober, js):
    """在每个 frame 里执行（只对真有视频的 frame 生效也无所谓）。"""
    for frame in prober._frames:
        ctx = prober._ctx(frame)
        if ctx is None:
            continue
        try:
            prober.conn.eval_js(js, 1.5, context_id=ctx)
        except Exception:
            pass


def main() -> int:
    needle = sys.argv[1] if len(sys.argv) > 1 else ""
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9222
    page = None
    for p in cdp.list_pages(port):
        if needle.lower() in (p["url"] + p["title"]).lower():
            page = p
            break
    if page is None:
        print("没找到页面:", needle)
        return 1
    print("页面:", page["title"], page["url"][:90])
    conn = cdp.CDPConnection(page["ws"], 3.0)
    conn.connect()
    prober = FrameProber(conn)
    try:
        show(prober, "初始")
        time.sleep(1.0)
        show(prober, "+1s")
        act(prober, PAUSE)
        time.sleep(0.8)
        show(prober, "暂停后")
        act(prober, PLAY)
        time.sleep(1.0)
        show(prober, "恢复后")
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
