# -*- coding: utf-8 -*-
"""开发期：诊断某个页面里的视频藏在哪一层 frame 里。

    python devframes.py [url关键词] [端口]
"""
import json
import sys

import cdp

PROBE = """
(function(){
  try{
    var vs=document.querySelectorAll('video'), out=[];
    for(var i=0;i<vs.length;i++){
      var v=vs[i];
      out.push({w:v.videoWidth,h:v.videoHeight,p:v.paused,rs:v.readyState,
                t:v.currentTime,d:isFinite(v.duration)?v.duration:null,
                src:(v.currentSrc||'').slice(0,60)});
    }
    return {count:out.length, videos:out, url:location.href,
            frames:document.querySelectorAll('iframe').length};
  }catch(e){ return {err:String(e)}; }
})()
"""


def main() -> int:
    needle = sys.argv[1] if len(sys.argv) > 1 else ""
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9222
    pages = cdp.list_pages(port)
    targets = [p for p in pages if needle.lower() in (p["url"] + p["title"]).lower()]
    if not targets:
        print("没有匹配的页面，现有：")
        for p in pages:
            print("  -", p["title"][:30], p["url"][:80])
        return 1
    for page in targets:
        print("=" * 70)
        print("页面:", page["title"], "\n ", page["url"][:110])
        conn = cdp.CDPConnection(page["ws"], 4.0)
        conn.connect()
        try:
            tree = conn.call("Page.getFrameTree", {}, 4.0)
            frames = []

            def walk(node, depth=0):
                f = node.get("frame", {})
                frames.append((depth, f))
                for child in node.get("childFrames") or []:
                    walk(child, depth + 1)

            walk(tree.get("frameTree") or {})
            for depth, f in frames:
                print("  " * depth + "- frame %s  %s" % (f.get("id", "")[:8], f.get("url", "")[:90]))
            print("  frames =", len(frames))

            for depth, f in frames:
                fid = f.get("id")
                try:
                    r = conn.call("Page.createIsolatedWorld",
                                  {"frameId": fid, "worldName": "tvm-probe",
                                   "grantUniveralAccess": True}, 3.0)
                    ctx = r.get("executionContextId")
                except Exception as exc:
                    print("  " * (depth + 1), "isolatedWorld 失败:", exc)
                    continue
                try:
                    res = conn.call("Runtime.evaluate",
                                    {"expression": PROBE, "contextId": ctx,
                                     "returnByValue": True}, 3.0)
                    val = (res.get("result") or {}).get("value")
                    print("  " * (depth + 1), "视频数:", json.dumps(val, ensure_ascii=False)[:300])
                except Exception as exc:
                    print("  " * (depth + 1), "求值失败:", exc)
        finally:
            conn.close()

    # iframe 类型的 target（跨进程 iframe 会单独成为一个 target）
    try:
        info = cdp.browser_version(port)
        b = cdp.CDPConnection(info["webSocketDebuggerUrl"], 4.0)
        b.connect()
        try:
            b.call("Target.setDiscoverTargets", {"discover": True}, 3.0)
            r = b.call("Target.getTargets", {}, 3.0)
            for t in r.get("targetInfos") or []:
                if t.get("type") in ("iframe", "webview", "other"):
                    print("target:", t.get("type"), t.get("title", "")[:30], t.get("url", "")[:90])
        finally:
            b.close()
    except Exception as exc:
        print("target 枚举失败:", exc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
