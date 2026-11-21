# -*- coding: utf-8 -*-
"""开发期自检：学习通「播到 N%」的百分比/提醒逻辑。

不需要浏览器、不需要开窗口；直接驱动 Channel 的进度状态机。

    py -3 devtest_progress.py
"""
import math
import os
import sys
import types

# app.py 顶部 import tkinter / winsound；这里用假模块，避免真的开窗口
tk = types.ModuleType("tkinter")
for n in ("Tk Frame Label Menu PhotoImage Toplevel BooleanVar IntVar DoubleVar Scale Spinbox Button Checkbutton").split():
    setattr(tk, n, object)
msg = types.ModuleType("tkinter.messagebox")
for n in ("showerror showinfo showwarning"):
    setattr(msg, n, lambda *a, **k: None)
ttk = types.ModuleType("tkinter.ttk")
ttk.Treeview = object
tk.messagebox = msg
tk.ttk = ttk
sys.modules["tkinter"] = tk
sys.modules["tkinter.messagebox"] = msg
sys.modules["tkinter.ttk"] = ttk

ws = types.ModuleType("winsound")
ws.Beep = lambda *a: None
ws.MessageBeep = lambda *a: None
sys.modules["winsound"] = ws

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app


def channel(url="https://mooc1.chaoxing.com/x", pct=90, only_cx=False):
    ch = app.Channel({"label": "v"})
    ch.page_url = url
    ch.set_progress_alert(True, pct, only_cx)
    return ch


def vid(k, t, d, w=1280, h=720, p=0, rs=4, s=""):
    return {"k": k, "t": t, "d": d, "p": p, "rs": rs,
            "w": w, "h": h, "e": 0, "m": 0, "s": s}


def res(vs, url="https://mooc1.chaoxing.com/x"):
    return {"ok": 1, "vs": vs, "title": "t", "url": url}


def main() -> int:
    fails = []

    def check(name, cond):
        print(("PASS " if cond else "FAIL ") + name)
        if not cond:
            fails.append(name)

    # 1) 正常播放：89% 不提醒，90% 提醒
    ch = channel()
    ch.duration = 600.0
    ch.play_time = 534.0          # 89%
    ch._prev_pct = None
    ch._update_progress()
    check("89% 不提醒", ch.progress_hit is False)
    ch.play_time = 540.0          # 90%
    ch._update_progress()
    check("90% 提醒", ch.progress_hit is True)

    # 2) 拖回 50%：必须复位（旧逻辑 50% > 45% 会继续闪+响）
    ch.play_time = 300.0
    ch._update_progress()
    check("拖回 50% 复位", ch.progress_hit is False)

    # 3) 从 50% 再播到 90%：重新提醒
    ch.play_time = 540.0
    ch._update_progress()
    check("从 50% 再播到 90% 重新提醒", ch.progress_hit is True)

    # 4) 一开始就在 95%：不立即提醒（等它先回到阈值下方再穿越）
    ch2 = channel()
    ch2.duration = 600.0
    ch2.play_time = 570.0
    ch2.progress_hit = False
    ch2._prev_pct = None
    ch2._update_progress()
    check("首次采样 95% 不提醒", ch2.progress_hit is False)
    ch2.play_time = 576.0
    ch2._update_progress()
    check("持续 96% 也不补提醒", ch2.progress_hit is False)

    # 5) duration/currentTime 异常：不崩、不报 100%
    ch3 = channel()
    ch3.duration = float("nan")
    ch3.play_time = 10.0
    ch3.progress_hit = True
    ch3._update_progress()
    check("duration=NaN 复位", ch3.progress_hit is False)
    check("duration=NaN 百分比为 0", ch3.progress_pct == 0.0)
    ch3.duration = 600.0
    ch3.play_time = float("nan")
    check("currentTime=NaN 百分比为 0", ch3.progress_pct == 0.0)

    # 6) 状态条：播放中最多显示 99%，不显示 100%
    fake = app.App.__new__(app.App)
    fake._show_watched = False
    chd = {"label": "视频2", "state": "playing", "play_time": 600.0,
           "watched_sec": 0.0, "note": "", "bad_sec": 0.0,
           "progress_hit": True, "progress_pct": 100.0}
    txt = fake._chan_text(chd, True, 5, False, True)
    check("播放中显示 99%", "99%" in txt)
    check("播放中不显示 100%", "100%" not in txt)

    # 7) 一个页面多个 video：取画面最大的正片，不取小广告
    ch4 = app.Channel({"label": "v"})
    ch4._last_update_at = 1000.0
    ch4.page_url = "https://mooc1.chaoxing.com/x"
    ch4.set_progress_alert(True, 90, False)
    ch4.update(res([
        vid("8:0", 1.0, 15.0, w=320, h=180),      # 小广告
        vid("8:1", 10.0, 600.0, w=1280, h=720),   # 正片
    ]), 1000.6)
    check("多 video 时取正片 duration=600", ch4.duration == 600.0)
    check("多 video 时取正片 play_time=10", ch4.play_time == 10.0)

    # 8) 只对学习通生效：B 站不复位/不提醒
    ch5 = channel(url="https://www.bilibili.com/x", only_cx=True)
    ch5.duration = 600.0
    ch5.play_time = 540.0
    ch5.progress_hit = True
    ch5._prev_pct = 89.0
    ch5._update_progress()
    check("非学习通不提醒", ch5.progress_hit is False)
    ch5.page_url = "https://mooc1.chaoxing.com/x"
    ch5.progress_hit = False
    ch5._prev_pct = 89.0
    ch5._update_progress()
    check("学习通上 90% 提醒", ch5.progress_hit is True)

    # 9) 换绑/换页：旧视频的提醒不能带过去
    ch6 = channel()
    ch6.duration = 600.0
    ch6.play_time = 540.0
    ch6.progress_hit = True
    ch6._prev_pct = 95.0
    ch6.reset_runtime()
    check("reset_runtime 清掉提醒", ch6.progress_hit is False)
    check("reset_runtime 清掉 prev_pct", ch6._prev_pct is None)

    # 10) 阈值 95 也按同一套逻辑
    ch7 = channel(pct=95)
    ch7.duration = 600.0
    ch7.play_time = 569.4          # 94.9%
    ch7._prev_pct = 94.0
    ch7._update_progress()
    check("94.9% 不提醒（阈值 95）", ch7.progress_hit is False)
    ch7.play_time = 570.0          # 95%
    ch7._update_progress()
    check("95% 提醒", ch7.progress_hit is True)

    # 11) 学习通换章节：视频源变了，新视频 95% 不能沿用旧提醒
    ch8 = app.Channel({"label": "v"})
    ch8._last_update_at = 1000.0
    ch8.page_url = "https://mooc1.chaoxing.com/x"
    ch8.set_progress_alert(True, 90, False)
    now = 1000.0
    for t in (539.4, 540.0):
        now += 0.6
        ch8.update(res([vid("8:0", t, 600.0, s="a.mp4")]), now)
    check("换源前 90% 已提醒", ch8.progress_hit is True)
    now += 0.6
    ch8.update(res([vid("9:0", 570.0, 600.0, s="b.mp4")]), now)
    check("换源后新视频 95% 不提醒", ch8.progress_hit is False)

    # 12) 没有 src 时，按时长突变识别新视频
    ch9 = app.Channel({"label": "v"})
    ch9._last_update_at = 2000.0
    ch9.page_url = "https://mooc1.chaoxing.com/x"
    ch9.set_progress_alert(True, 90, False)
    now = 2000.0
    for t in (539.4, 540.0):
        now += 0.6
        ch9.update(res([vid("8:0", t, 600.0)]), now)
    check("时长变化前已提醒", ch9.progress_hit is True)
    now += 0.6
    ch9.update(res([vid("9:0", 285.0, 300.0)]), now)
    check("时长突变后新视频 95% 不提醒", ch9.progress_hit is False)

    print()
    if fails:
        print("FAILED:", len(fails))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
