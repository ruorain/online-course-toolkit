# -*- coding: utf-8 -*-
"""开发期自检：验证「本次启动已刷时长」的累计口径。

不需要浏览器、不需要开窗口；直接喂假 CDP 数据驱动 Channel。

    py -3 devtest_watched.py
"""
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


def vid(t, p=0, rs=4, d=600.0, k="8:0"):
    return {"k": k, "t": t, "p": p, "rs": rs, "d": d, "w": 1280, "h": 720, "e": 0, "m": 0}


def res(vs):
    return {"ok": 1, "vs": vs, "title": "t", "url": "https://mooc1.chaoxing.com/x"}


def run(ch, v, now):
    ch.update(res([v]), now)
    return ch


def close(a, b, eps=0.05):
    return abs(a - b) <= eps


def main() -> int:
    fails = []

    def check(name, cond):
        print(("PASS " if cond else "FAIL ") + name)
        if not cond:
            fails.append(name)

    # 1) 1x 播放：第一次没有 prev 不计，之后每次 0.6s
    ch = app.Channel({"label": "v1"})
    ch._last_update_at = 1000.0
    t = 0.0
    now = 1000.0
    for _ in range(11):
        t += 0.6
        now += 0.6
        run(ch, vid(t), now)
    check("1x 播放累计 6.0 秒", close(ch.watched_sec, 6.0))
    check("1x 播放状态为 playing", ch.state == "playing")

    # 2) 2x 播放：currentTime 每拍走 1.2s，按课程时长计
    ch = app.Channel({"label": "v2"})
    ch._last_update_at = 1000.0
    t = 0.0
    now = 1000.0
    for _ in range(11):
        t += 1.2
        now += 0.6
        run(ch, vid(t), now)
    check("2x 播放累计 12.0 秒（按视频秒数）", close(ch.watched_sec, 12.0))

    # 3) 暂停：不增长
    before = ch.watched_sec
    now += 0.6
    run(ch, vid(t, p=1), now)
    check("暂停不累计", close(ch.watched_sec, before))
    check("暂停状态为 stopped", ch.state == "stopped")

    # 4) 拖动 +300s：只按实际耗时封顶计入
    now += 0.6
    run(ch, vid(t + 300.0), now)
    check("拖动 +300s 只计封顶 3.6s", close(ch.watched_sec, before + 3.6))

    # 5) 采样间隔 3.5s：状态可能仍是 buffering，但计时不能停
    ch = app.Channel({"label": "v3"})
    ch._last_update_at = 1000.0
    now = 1000.0
    run(ch, vid(0.6), now)
    base = ch.watched_sec
    now += 3.5
    run(ch, vid(4.1), now)
    check("慢采样时仍累计 3.5s", close(ch.watched_sec, base + 3.5))
    check("慢采样状态仍按原逻辑（buffering）", ch.state == "buffering")

    # 6) 换 key（prev=None）：那一拍不计，下一拍继续
    ch = app.Channel({"label": "v4"})
    ch._last_update_at = 1000.0
    now = 1000.0
    run(ch, vid(10.0), now)
    base = ch.watched_sec
    now += 0.6
    run(ch, vid(10.6, k="9:0"), now)
    check("换 key 那一拍不计", close(ch.watched_sec, base))
    now += 0.6
    run(ch, vid(11.2, k="9:0"), now)
    check("换 key 后下一拍继续计 0.6", close(ch.watched_sec, base + 0.6))

    # 7) 新实例从 0 开始（本次启动语义）
    check("新实例归零", app.Channel({"label": "v5"}).watched_sec == 0.0)

    # 8) 循环回开头：负增量不计
    ch = app.Channel({"label": "v6"})
    ch._last_update_at = 1000.0
    now = 1000.0
    run(ch, vid(599.0), now)
    now += 0.6
    run(ch, vid(0.2), now)
    check("循环回开头不产生负累计", ch.watched_sec >= 0.0)

    # 9) 状态条文本：每路分开显示，不合并成一个总数
    fake = app.App.__new__(app.App)
    fake._show_watched = True

    def fake_ch(label, watched):
        return {"label": label, "state": "playing", "play_time": 12.0,
                "watched_sec": watched, "note": "", "bad_sec": 0.0,
                "progress_hit": False, "progress_pct": 0.0}

    ta = fake._chan_text(fake_ch("视频1", 3723.0), False, 5)
    tb = fake._chan_text(fake_ch("视频2", 2156.0), False, 5)
    check("视频1 单独显示 01:02:03", "已刷 01:02:03" in ta)
    check("视频2 单独显示 00:35:56", "已刷 00:35:56" in tb)
    check("两路数值不串", "01:02:03" in ta and "00:35:56" in tb)
    fake._show_watched = False
    check("关掉开关不显示已刷", "已刷" not in fake._chan_text(fake_ch("视频1", 3723.0), False, 5))

    print()
    if fails:
        print("FAILED:", len(fails))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
