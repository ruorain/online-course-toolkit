# -*- coding: utf-8 -*-
"""穿透 iframe 找视频。

很多站点（超星学习通、智慧树、各类网课平台）把播放器放在一两层 iframe 里，
主页面 document.querySelectorAll('video') 自然是 0 个。
这里用 CDP 的 frame tree + 每个 frame 的隔离世界，把所有子 frame 里的 video
一起读出来；跨进程 iframe（OOPIF）则通过 auto-attach 的 session 读取。
"""

from __future__ import annotations

import time

# 在每个 frame 里跑的探针：只读 DOM，不依赖页面自己的 JS 变量
PROBE_JS = r"""
(function () {
  try {
    var out = [];
    var vs = document.querySelectorAll('video');
    for (var i = 0; i < vs.length; i++) {
      var v = vs[i], t = 0, d = 0, s = '';
      try { t = v.currentTime || 0; } catch (e) {}
      try { d = v.duration; } catch (e) {}
      try { s = String(v.currentSrc || v.src || '').split('?')[0].slice(0, 300); } catch (e) {}
      out.push({
        w: v.videoWidth | 0,
        h: v.videoHeight | 0,
        p: v.paused ? 1 : 0,
        e: v.ended ? 1 : 0,
        rs: v.readyState | 0,
        t: t,
        d: (typeof d === 'number' && isFinite(d)) ? d : 0,
        m: v.muted ? 1 : 0,
        s: s
      });
    }
    return {
      ok: 1, vs: out, n: out.length,
      title: document.title || '',
      url: String(location.href),
      hidden: document.hidden ? 1 : 0
    };
  } catch (err) {
    return { ok: 0, err: String(err) };
  }
})()
"""


class FrameProber:
    """一个页面的"所有 frame 里的视频"探测器。非线程安全（跟着一条连接用）。"""

    def __init__(self, conn, refresh_sec: float = 5.0):
        self.conn = conn
        self._frames = []          # [{"id","url","ctx"}]
        self._last_refresh = 0.0
        self._refresh_sec = refresh_sec
        self._sessions = set()     # OOPIF 的 sessionId
        self._auto_tried = False

    # -- frame 列表 ------------------------------------------------------- #
    def reset(self):
        self._frames = []
        self._last_refresh = 0.0
        self._sessions = set()

    def refresh(self):
        try:
            tree = self.conn.call("Page.getFrameTree", {}, 2.0)
        except Exception:
            self._frames = []
            self._last_refresh = time.monotonic()
            return
        frames = []

        def walk(node):
            f = node.get("frame") or {}
            frames.append({"id": f.get("id"), "url": f.get("url", "")})
            for ch in node.get("childFrames") or []:
                walk(ch)

        walk(tree.get("frameTree") or {})
        old = {f["id"]: f for f in self._frames}
        for f in frames:
            prev = old.get(f["id"])
            f["ctx"] = prev.get("ctx") if prev else None
        self._frames = frames
        self._last_refresh = time.monotonic()

    def _ctx(self, frame):
        if frame.get("ctx") is not None:
            return frame["ctx"]
        try:
            r = self.conn.call(
                "Page.createIsolatedWorld",
                {
                    "frameId": frame["id"],
                    "worldName": "tvm-probe",
                    # CDP 里这个字段名确实少一个字母，照抄才生效
                    "grantUniveralAccess": True,
                },
                1.5,
            )
            frame["ctx"] = r.get("executionContextId")
        except Exception:
            frame["ctx"] = None
        return frame["ctx"]

    # -- OOPIF ------------------------------------------------------------ #
    def _enable_auto_attach(self):
        if self._auto_tried:
            return
        self._auto_tried = True

        def on_event(ev):
            method = ev.get("method")
            params = ev.get("params") or {}
            if method == "Target.attachedToTarget":
                sid = params.get("sessionId")
                if sid:
                    self._sessions.add(sid)
            elif method == "Target.detachedFromTarget":
                self._sessions.discard(params.get("sessionId"))

        self.conn.on_event = on_event
        try:
            self.conn.call(
                "Target.setAutoAttach",
                {"autoAttach": True, "waitForDebuggerOnStart": False, "flatten": True},
                1.5,
            )
        except Exception:
            pass

    # -- 主入口 ----------------------------------------------------------- #
    def probe(self, timeout: float = 1.2) -> dict:
        now = time.monotonic()
        if not self._frames or now - self._last_refresh > self._refresh_sec:
            self.refresh()

        merged = []
        base = None
        if self._frames:
            self._enable_auto_attach()
            budget = 12  # 广告站可能有几十个 frame，够用就行
            for fi, frame in enumerate(self._frames):
                url = (frame.get("url") or "").strip()
                # about:blank 基本是填充广告位，跳过省 CPU（顶层永远要探）
                if fi > 0 and (not url or url == "about:blank"):
                    continue
                if budget <= 0:
                    break
                budget -= 1
                ctx = self._ctx(frame)
                if ctx is None:
                    continue
                try:
                    val = self.conn.eval_js(PROBE_JS, timeout, context_id=ctx)
                except Exception:
                    frame["ctx"] = None  # 上下文被导航销毁，下轮重建
                    continue
                if not isinstance(val, dict) or not val.get("ok"):
                    continue
                if base is None:
                    base = val
                for i, v in enumerate(val.get("vs") or []):
                    v["k"] = "%d:%d" % (fi, i)
                    v["frame"] = frame.get("url", "")
                    merged.append(v)
            for sid in list(self._sessions):
                try:
                    val = self.conn.eval_js(PROBE_JS, timeout, session_id=sid)
                except Exception:
                    continue
                if not isinstance(val, dict) or not val.get("ok"):
                    continue
                for i, v in enumerate(val.get("vs") or []):
                    v["k"] = "%s:%d" % (sid[-8:], i)
                    v["frame"] = val.get("url", "")
                    merged.append(v)
        if base is None:
            # frame tree 拿不到时退回顶层页面
            try:
                val = self.conn.eval_js(PROBE_JS, timeout)
            except Exception:
                val = None
            base = val if isinstance(val, dict) and val.get("ok") else {
                "ok": 0, "vs": [], "n": 0, "title": "", "url": "", "hidden": 0,
            }
            for i, v in enumerate(base.get("vs") or []):
                v["k"] = "t:%d" % i
                v["frame"] = base.get("url", "")
                merged.append(v)

        out = dict(base)
        out["vs"] = merged
        out["n"] = len(merged)
        out["frames"] = len(self._frames) or 1
        return out


def count_videos(prober: FrameProber) -> int:
    """真正有画面的视频个数（给窗口列表用）。"""
    res = prober.probe(1.2)
    n = 0
    for v in res.get("vs") or []:
        if (v.get("w") or 0) > 0 or (v.get("rs") or 0) >= 2 or (v.get("d") or 0) > 0:
            n += 1
    return n
