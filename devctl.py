# -*- coding: utf-8 -*-
"""开发期小工具：对指定页面执行 暂停/播放，方便验证监控条。

    python devctl.py pause [url片段]
    python devctl.py play  [url片段]
"""
import sys

import cdp

CMD = {"pause": "v.pause()", "play": "v.play()"}


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in CMD:
        print(__doc__)
        return 1
    action = sys.argv[1]
    needle = sys.argv[2] if len(sys.argv) > 2 else ""
    js = "var v=document.querySelector('video'); if(v){%s;} [v&&v.paused, v&&v.currentTime]" % CMD[action]
    for p in cdp.list_pages(9222):
        if needle and needle not in p["url"] and needle not in p["title"]:
            continue
        conn = cdp.CDPConnection(p["ws"], 3.0)
        conn.connect()
        try:
            print("%-28s %s" % (p["url"][-28:], conn.eval_js(js)))
        finally:
            conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
