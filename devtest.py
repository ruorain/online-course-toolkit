# -*- coding: utf-8 -*-
"""开发期自检：直接驱动 Channel 状态机，验证 播放/暂停/循环 的判定。

    python devtest.py [端口]
"""
import sys
import time

import cdp
import app


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
    pages = {p["id"]: p for p in cdp.list_pages(port)}
    if not pages:
        print("没有可用的页面")
        return 1
    tid = list(pages)[0]
    page = pages[tid]
    print("页面:", page["title"], page["url"][-40:])

    ch = app.Channel({"label": "视频1", "url_hint": page["url"], "title_hint": page["title"]})
    conn = cdp.CDPConnection(page["ws"], 3.0)
    conn.connect()

    def step(tag, n=4):
        print("--", tag)
        for i in range(n):
            try:
                res = conn.eval_js(app.PROBE_JS, 2.0)
            except Exception as exc:
                print("   求值失败:", exc)
                return
            ch.update(res, time.monotonic())
            print("   %-8s note=%-12s t=%6.2f bad=%4.1fs videos=%s" % (
                ch.state, ch.note, ch.play_time, ch.bad_sec,
                [(v["w"], v["h"], v["p"], round(v["t"], 2)) for v in res.get("vs", [])]))
            time.sleep(0.5)

    step("初始（应当 playing）")
    conn.eval_js("var v=document.querySelector('video'); v.pause(); 'ok'")
    step("暂停后（应当 stopped）", 3)
    conn.eval_js("var v=document.querySelector('video'); v.play(); 'ok'")
    step("恢复播放（应当 playing）", 3)
    conn.close()
    print("完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
