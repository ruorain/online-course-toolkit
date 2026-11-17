# -*- coding: utf-8 -*-
"""置顶视频播放监控条

一行文字，永远显示在最上层，实时汇报两个浏览器窗口里的视频是否在播放；
一旦某个窗口停止播放（暂停 / 播完 / 卡住 / 浏览器断开），文字醒目闪烁并发出提示音。

检测原理：
    本程序用独立配置目录启动 Edge/Chrome，并打开调试端口。
    通过 CDP 在页面里读取 <video> 的 currentTime / paused / readyState，
    用 currentTime 是否在推进来判断"真的在播"，比看画面像素更准。
"""

from __future__ import annotations

import ctypes
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
import tkinter as tk
import traceback
import urllib.parse
import winsound
from tkinter import messagebox, ttk

import cdp
import frames
from frames import PROBE_JS, FrameProber

APP_NAME = "TopVideoMonitor"


def _app_dir() -> str:
    """单文件 exe（PyInstaller）里 __file__ 指向临时解包目录，得用 exe 所在目录。"""
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


APP_DIR = _app_dir()
BUNDLE_DIR = getattr(sys, "_MEIPASS", APP_DIR)  # onefile 解包目录
DATA_DIR = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), APP_NAME)
CONFIG_PATH = os.path.join(DATA_DIR, "config.json")
LOG_PATH = os.path.join(DATA_DIR, "error.log")
def _resource(name: str) -> str:
    for base in (APP_DIR, BUNDLE_DIR):
        path = os.path.join(base, name)
        if os.path.isfile(path):
            return path
    return os.path.join(APP_DIR, name)


START_PAGE = _resource("测试页面.html")
HELP_PAGE = _resource("使用说明.html")

# ---------------------------------------------------------------- 配色 / 常量
BG = "#1b1d23"
BORDER = "#3b4150"
FG = "#e8ebf2"
FG_DIM = "#9aa1b1"
C_PLAY = "#2ee59d"
C_STOP = "#ff5d5d"
C_WARN = "#ffb020"
C_OFF = "#ff8a80"
C_IDLE = "#8a90a0"
FLASH_A = "#ff2d55"
FLASH_B = "#ffe14d"

STATE_PLAYING = "playing"
STATE_STOPPED = "stopped"
STATE_BUFFERING = "buffering"
STATE_NOVIDEO = "novideo"
STATE_OFFLINE = "offline"
STATE_UNBOUND = "unbound"

BAD_STATES = (STATE_STOPPED, STATE_BUFFERING, STATE_NOVIDEO, STATE_OFFLINE)

ICON = {
    STATE_PLAYING: "●",
    STATE_STOPPED: "▲",
    STATE_BUFFERING: "◐",
    STATE_NOVIDEO: "○",
    STATE_OFFLINE: "✕",
    STATE_UNBOUND: "…",
}
TEXT = {
    STATE_PLAYING: "播放中",
    STATE_STOPPED: "已停止",
    STATE_BUFFERING: "卡住了",
    STATE_NOVIDEO: "无视频",
    STATE_OFFLINE: "未连接",
    STATE_UNBOUND: "未指定",
}


# ---------------------------------------------------------------- 小工具
def init_dpi() -> float:
    """开启 DPI 感知，返回缩放系数（96dpi=1.0）。"""
    scale = 1.0
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
    try:
        scale = ctypes.windll.user32.GetDpiForSystem() / 96.0
    except Exception:
        pass
    return max(1.0, min(3.0, scale))


def fmt_time(sec: float) -> str:
    sec = int(max(0.0, sec or 0))
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    return "%d:%02d:%02d" % (h, m, s) if h else "%02d:%02d" % (m, s)


def atomic_write_json(path: str, data) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def log_note(msg: str) -> None:
    """往 error.log 追加一行（排查启动类问题用）。"""
    try:
        os.makedirs(DATA_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write("[%s] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except Exception:
        pass


def log_exception(where: str):
    try:
        os.makedirs(DATA_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write("\n===== %s %s =====\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), where))
            f.write(traceback.format_exc())
    except Exception:
        pass


def find_browser() -> str:
    """找一个 Chromium 内核浏览器（Edge 优先，其次 Chrome）。"""
    pf = os.environ.get("ProgramFiles", r"C:\Program Files")
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    local = os.environ.get("LOCALAPPDATA", "")
    cands = [
        os.path.join(pf86, r"Microsoft\Edge\Application\msedge.exe"),
        os.path.join(pf, r"Microsoft\Edge\Application\msedge.exe"),
        os.path.join(local, r"Microsoft\Edge\Application\msedge.exe"),
        os.path.join(pf, r"Google\Chrome\Application\chrome.exe"),
        os.path.join(pf86, r"Google\Chrome\Application\chrome.exe"),
        os.path.join(local, r"Google\Chrome\Application\chrome.exe"),
    ]
    for p in cands:
        if p and os.path.isfile(p):
            return p
    return ""


def default_profile_dir() -> str:
    return os.path.join(os.environ.get("LOCALAPPDATA") or DATA_DIR, APP_NAME, "browser-profile")


DEFAULT_CONFIG = {
    "port": 9222,
    "browser_exe": "",
    "profile_dir": "",
    "threshold_sec": 5,      # 停止多少秒后开始闪烁报警
    "sound": True,
    "progress_alert": True,  # 播放到指定百分比时也提醒（网课刷课用）
    "progress_percent": 90,  # 提醒百分比，1~99
    "progress_only_chaoxing": True,  # 进度提醒只对学习通（*.chaoxing.com）生效
    "beep_interval": 8,      # 提示音重复间隔（秒）
    "flash_ms": 450,         # 闪烁半周期（毫秒）
    "font_px": 15,           # 字号（96dpi 下的像素）
    "opacity": 1.0,
    "topmost": True,
    "autostart": False,
    "bar": {"x": None, "y": 40},
    "channels": [
        {"label": "视频1", "url_hint": "", "title_hint": ""},
        {"label": "视频2", "url_hint": "", "title_hint": ""},
    ],
}


def load_config() -> dict:
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))  # 深拷贝
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            saved = json.load(f)
        for k, v in saved.items():
            if k == "channels" and isinstance(v, list):
                for i, ch in enumerate(v[:2]):
                    if isinstance(ch, dict):
                        cfg["channels"][i].update(ch)
            elif k == "bar" and isinstance(v, dict):
                cfg["bar"].update(v)
            else:
                cfg[k] = v
    except FileNotFoundError:
        pass
    except Exception:
        log_exception("load_config")
    if not cfg.get("browser_exe"):
        cfg["browser_exe"] = find_browser()
    if not cfg.get("profile_dir"):
        cfg["profile_dir"] = default_profile_dir()
    return cfg


# ---------------------------------------------------------------- 通道状态机
class Channel:
    """一个被监控的浏览器窗口/标签页。"""

    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.target_id = ""
        self.state = STATE_UNBOUND
        self.note = ""
        self.page_title = ""
        self.page_url = ""
        self.play_time = 0.0
        self.duration = 0.0
        self.muted = False
        self.state_since = time.monotonic()
        self.bad_since = None
        self._last_t = {}
        self._changed_at = time.monotonic()
        # 进度提醒（播到 N% 时叫一声，学习通刷课用）
        self.progress_hit = False
        self._prog_enabled = False
        self._prog_percent = 90.0
        self._prog_only_chaoxing = True

    # -- 绑定 ------------------------------------------------------------- #
    @property
    def bound(self) -> bool:
        return bool(self.cfg.get("url_hint") or self.cfg.get("title_hint"))

    def reset_runtime(self):
        self.target_id = ""
        self.state = STATE_UNBOUND
        self.note = ""
        self._last_t = {}
        self.bad_since = None
        self.state_since = time.monotonic()

    def resolve(self, pages: list, used: set) -> None:
        """把配置里的 url/标题 提示解析成本次运行的 target id。"""
        if self.target_id and self.target_id in pages and self.target_id not in used:
            used.add(self.target_id)
            return
        self.target_id = ""
        if not self.bound:
            return
        hint_url = (self.cfg.get("url_hint") or "").strip()
        hint_title = (self.cfg.get("title_hint") or "").strip()
        best, best_score = None, 0
        for tid, p in pages.items():
            if tid in used:
                continue
            score = 0
            if hint_url and p["url"] == hint_url:
                score = 5
            elif hint_url and _norm_url(p["url"]) == _norm_url(hint_url):
                score = 4
            elif hint_title and p["title"] == hint_title:
                score = 3
            elif hint_title and hint_title[:12] and hint_title[:12] in p["title"]:
                score = 2
            if score > best_score:
                best, best_score = p, score
        if best is not None and best_score >= 2:
            self.target_id = best["id"]
            used.add(best["id"])
            self.page_title = best["title"]
            self.page_url = best["url"]
            if self.state == STATE_UNBOUND:
                self._set_state(STATE_OFFLINE, time.monotonic(), "")

    # -- 状态 ------------------------------------------------------------- #
    def _set_state(self, state: str, now: float, note: str):
        if state != self.state or note != self.note:
            if state != self.state:
                self.state_since = now
            self.state = state
            self.note = note
            self._changed_at = now
        if state in BAD_STATES:
            if self.bad_since is None:
                self.bad_since = now
        else:
            self.bad_since = None

    @property
    def bad_sec(self) -> float:
        if self.bad_since is None:
            return 0.0
        return max(0.0, time.monotonic() - self.bad_since)

    def set_unbound(self, now: float):
        self.reset_runtime()
        self._set_state(STATE_UNBOUND, now, "")

    def mark_offline(self, now: float, note: str = "窗口已关闭或浏览器断开"):
        self._last_t = {}
        self._set_state(STATE_OFFLINE, now, note)

    def update(self, res, now: float):
        if not isinstance(res, dict) or not res.get("ok"):
            self.mark_offline(now, "页面无法读取")
            return
        self.page_title = res.get("title") or self.page_title
        self.page_url = res.get("url") or self.page_url
        vids = res.get("vs") or []
        # 只统计真正的视频元素：有画面、或已加载出元数据（排除 0x0 的预加载占位）
        cand = [
            (v.get("k", i), v) for i, v in enumerate(vids)
            if (v.get("w") or 0) > 0 or (v.get("rs") or 0) >= 2 or (v.get("d") or 0) > 0
        ]
        prev_map = dict(self._last_t)
        for i, v in enumerate(vids):
            self._last_t[v.get("k", i)] = v.get("t") or 0.0
        if len(self._last_t) > 64:  # 防止 iframe 换来换去把记录撑大
            alive = {v.get("k", i) for i, v in enumerate(vids)}
            self._last_t = {k: t for k, t in self._last_t.items() if k in alive}
        if not cand:
            self.play_time = 0.0
            self._set_state(STATE_NOVIDEO, now, "页面里没有视频")
            return

        playing, stalled, ended = [], [], []
        for i, v in cand:
            if v.get("e"):
                ended.append((i, v))
                continue
            if v.get("p"):
                continue
            if (v.get("rs") or 0) < 2:
                stalled.append((i, v))
                continue
            prev = prev_map.get(i)
            cur_t = v.get("t") or 0.0
            dur = v.get("d") or 0.0
            # 关键：用 currentTime 是否推进来判断"真的在播"
            if prev is None:
                playing.append((i, v))
            elif 0.02 < cur_t - prev < 3.0:
                playing.append((i, v))
            elif dur > 0 and prev > dur - 1.5 and cur_t < 1.5:
                playing.append((i, v))  # 循环播放回到开头
            else:
                stalled.append((i, v))

        if playing:
            i, v = playing[0]
            self.play_time = v.get("t") or 0.0
            self.duration = v.get("d") or 0.0
            self.muted = bool(v.get("m"))
            self._set_state(STATE_PLAYING, now, "")
            self._update_progress()
        elif stalled:
            self._set_state(STATE_BUFFERING, now, "时间不走（缓冲/卡住）")
        elif ended:
            self._set_state(STATE_STOPPED, now, "播放结束")
        else:
            self._set_state(STATE_STOPPED, now, "已暂停")

    def set_progress_alert(self, enabled: bool, percent: float, only_chaoxing: bool = True):
        """由监控线程每轮同步配置。"""
        self._prog_enabled = bool(enabled)
        self._prog_only_chaoxing = bool(only_chaoxing)
        try:
            self._prog_percent = max(1.0, min(99.0, float(percent)))
        except Exception:
            self._prog_percent = 90.0
        if not self._prog_enabled:
            self.progress_hit = False

    def _is_chaoxing(self) -> bool:
        """当前页面是不是学习通（*.chaoxing.com）。"""
        try:
            host = (urllib.parse.urlparse(self.page_url or "").hostname or "").lower()
        except Exception:
            return False
        return host == "chaoxing.com" or host.endswith(".chaoxing.com")

    def _update_progress(self):
        if not self._prog_enabled or self.duration <= 0:
            self.progress_hit = False
            return
        if self._prog_only_chaoxing and not self._is_chaoxing():
            # 只认学习通，其他站点不做进度提醒
            self.progress_hit = False
            return
        pct = self.play_time / self.duration * 100.0
        if pct >= self._prog_percent:
            self.progress_hit = True
        elif pct < self._prog_percent * 0.5:
            # 从头/从很早的位置重新播了，重新武装
            self.progress_hit = False

    @property
    def progress_pct(self) -> float:
        if self.duration <= 0:
            return 0.0
        return max(0.0, min(100.0, self.play_time / self.duration * 100.0))

    def snapshot(self) -> dict:
        return {
            "label": self.cfg.get("label") or "视频",
            "state": self.state,
            "note": self.note,
            "bad_sec": self.bad_sec,
            "play_time": self.play_time,
            "duration": self.duration,
            "muted": self.muted,
            "title": self.page_title,
            "url": self.page_url,
            "bound": self.bound,
            "target_id": self.target_id,
            "progress_hit": self.progress_hit,
            "progress_percent": self._prog_percent,
            "progress_pct": self.progress_pct,
        }


def _norm_url(url: str) -> str:
    url = (url or "").split("#", 1)[0]
    return url


# ---------------------------------------------------------------- 监控线程
class Monitor(threading.Thread):
    def __init__(self, cfg: dict, out_q: "queue.Queue"):
        super().__init__(daemon=True, name="monitor")
        self.cfg = cfg
        self.out_q = out_q
        self.stop_evt = threading.Event()
        self.lock = threading.Lock()
        self.channels = [Channel(c) for c in cfg["channels"]]
        self.pages = {}
        self.pages_list = []
        self.connected = False
        self.version = ""
        self.error = ""
        self._conns = {}
        self._probers = {}
        self._restore_tried = set()

    # -- 对外 ------------------------------------------------------------- #
    def shutdown(self):
        self.stop_evt.set()

    def snapshot(self) -> dict:
        with self.lock:
            return {
                "connected": self.connected,
                "version": self.version,
                "error": self.error,
                "channels": [c.snapshot() for c in self.channels],
                "pages": list(self.pages_list),
            }

    def bind(self, index: int, page: dict = None):
        """把第 index 个通道绑到某个页面（page=None 表示解绑）。"""
        ch = self.channels[index]
        if page is None:
            ch.cfg["url_hint"] = ""
            ch.cfg["title_hint"] = ""
        else:
            ch.cfg["url_hint"] = page.get("url", "")
            ch.cfg["title_hint"] = page.get("title", "")
        ch.reset_runtime()
        return ch.cfg

    # -- 主循环 ----------------------------------------------------------- #
    def run(self):
        while not self.stop_evt.is_set():
            try:
                self._tick()
            except Exception:
                self.error = "监控内部错误"
                log_exception("monitor.tick")
            self.stop_evt.wait(0.6)
        self._close_all()

    def _close_all(self):
        for conn in self._conns.values():
            try:
                conn.close()
            except Exception:
                pass
        self._conns.clear()
        self._probers.clear()

    def _tick(self):
        now = time.monotonic()
        port = int(self.cfg.get("port", 9222))
        pages, connected, version = {}, False, self.version
        try:
            info = cdp.browser_version(port, 1.0)
            connected = True
            version = info.get("Browser") or version
            for p in cdp.list_pages(port, 1.2):
                pages[p["id"]] = p
            self.error = ""
        except cdp.CDPError as exc:
            connected = False
            self.error = "浏览器未连接"
            if self._conns:
                self._close_all()

        with self.lock:
            self.connected = connected
            self.version = version
            self.pages = pages
            self.pages_list = list(pages.values())

        used = set()
        for ch in self.channels:
            ch.resolve(pages, used)
            ch.set_progress_alert(
                bool(self.cfg.get("progress_alert", True)),
                float(self.cfg.get("progress_percent", 90) or 90),
                bool(self.cfg.get("progress_only_chaoxing", True)),
            )

        for ch in self.channels:
            if not ch.bound:
                ch.set_unbound(now)
                continue
            if not connected:
                ch.mark_offline(now, "浏览器未连接")
                continue
            if not ch.target_id:
                ch.mark_offline(now, "找不到对应窗口（可在设置里重新指定）")
                continue
            page = pages.get(ch.target_id)
            if page is None:
                self._drop_conn(ch.target_id)
                ch.mark_offline(now, "窗口已关闭")
                continue
            conn = self._conns.get(ch.target_id)
            if conn is None or conn.closed or not page.get("ws"):
                self._drop_conn(ch.target_id)
                if not page.get("ws"):
                    ch.mark_offline(now, "无法连接该标签页")
                    continue
                try:
                    conn = cdp.CDPConnection(page["ws"], 2.0)
                    conn.connect()
                    self._conns[ch.target_id] = conn
                    self._probers[ch.target_id] = FrameProber(conn)
                except Exception:
                    ch.mark_offline(now, "无法连接该标签页")
                    continue
            prober = self._probers.get(ch.target_id)
            if prober is None:
                prober = self._probers[ch.target_id] = FrameProber(conn)
            try:
                res = prober.probe(1.2)   # 会穿透 iframe
            except Exception:
                self._drop_conn(ch.target_id)
                ch.mark_offline(now, "读取页面失败，正在重连")
                continue
            ch.update(res, now)

        with self.lock:
            snap = {
                "connected": self.connected,
                "version": self.version,
                "error": self.error,
                "channels": [c.snapshot() for c in self.channels],
                "pages": list(self.pages_list),
            }
        try:
            while self.out_q.full():
                self.out_q.get_nowait()
            self.out_q.put_nowait(snap)
        except Exception:
            pass

    def _drop_conn(self, tid: str):
        conn = self._conns.pop(tid, None)
        self._probers.pop(tid, None)
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass


# ---------------------------------------------------------------- 提示音
class Alerter(threading.Thread):
    def __init__(self, cfg: dict):
        super().__init__(daemon=True, name="alerter")
        self.cfg = cfg
        self._active = threading.Event()
        self._stop = threading.Event()

    def set_active(self, on: bool):
        if on:
            self._active.set()
        else:
            self._active.clear()

    def preview(self):
        threading.Thread(target=self._pattern, daemon=True).start()

    def shutdown(self):
        self._stop.set()
        self._active.clear()

    def run(self):
        while not self._stop.is_set():
            if not self._active.is_set():
                self._stop.wait(0.1)
                continue
            if self.cfg.get("sound", True):
                self._pattern()
            interval = max(2.0, float(self.cfg.get("beep_interval", 8) or 8))
            end = time.monotonic() + interval
            while time.monotonic() < end:
                if self._stop.is_set() or not self._active.is_set():
                    break
                time.sleep(0.06)

    @staticmethod
    def _pattern():
        try:
            winsound.Beep(1050, 130)
            time.sleep(0.05)
            winsound.Beep(1450, 130)
            time.sleep(0.05)
            winsound.Beep(1050, 130)
            time.sleep(0.05)
            winsound.Beep(1450, 220)
        except Exception:
            try:
                winsound.MessageBeep(winsound.MB_ICONHAND)
            except Exception:
                pass


# ---------------------------------------------------------------- 主界面
class App:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.scale = init_dpi()
        self.q = queue.Queue(maxsize=8)
        self.monitor = Monitor(cfg, self.q)
        self.alerter = Alerter(cfg)
        self.last_snap = None
        self._flash_on = False
        self._alerting = False
        self._settings = None
        self._build_ui()
        self.monitor.start()
        self.alerter.start()
        self.root.after(150, self._tick)
        self.root.after(400, self._flash_tick)

    CLOSE_FG = "#8a90a0"
    CLOSE_FG_HOT = "#ff5d5d"

    # -- 界面 ------------------------------------------------------------- #
    def _font(self, size_px=None, bold=True) -> tuple:
        px = int(round((size_px or self.cfg.get("font_px", 15)) * self.scale))
        return ("Microsoft YaHei UI", -max(9, px), "bold" if bold else "normal")

    def _build_ui(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title("视频播放监控")
        # pythonw 没有控制台，Tk 回调里的异常默认会静默吞掉，这里统一记日志
        self.root.report_callback_exception = self._on_tk_error
        try:  # 窗口/任务栏图标
            icon_path = os.path.join(APP_DIR, "icon.png")
            if os.path.isfile(icon_path):
                self._icon_img = tk.PhotoImage(file=icon_path)
                self.root.iconphoto(True, self._icon_img)
        except Exception:
            pass
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", bool(self.cfg.get("topmost", True)))
        self.root.attributes("-alpha", float(self.cfg.get("opacity", 1.0)))

        self.outer = tk.Frame(self.root, bg=BORDER)
        self.outer.pack(fill="both", expand=True)
        self.inner = tk.Frame(self.outer, bg=BG)
        self.inner.pack(fill="both", expand=True, padx=1, pady=1)

        self.labels = []
        self.seps = []
        for i in range(2):
            if i:
                sep = tk.Label(
                    self.inner, text="│", bg=BG, fg="#565d6d",
                    font=("Microsoft YaHei UI", -int(13 * self.scale)),
                )
                sep.pack(side="left", fill="y")
                self.seps.append(sep)
                self._bind_drag(sep)
            lbl = tk.Label(
                self.inner, text="… 视频%d 未指定" % (i + 1), bg=BG, fg=C_IDLE,
                font=self._font(), padx=int(7 * self.scale), pady=int(2 * self.scale),
                anchor="center",
            )
            lbl.pack(side="left", fill="y")
            self._bind_drag(lbl)
            self.labels.append(lbl)

        # 「我知道了」：报警时出现，点一下停止闪烁和响铃（文字保留，恢复播放后自动重新武装）
        self._ack_btn_shown = False
        self._acked = set()
        self._alert_idx = set()
        self.ack_btn = tk.Label(
            self.inner, text="我知道了", bg="#ffd166", fg="#1b1d23",
            font=self._font(), padx=int(8 * self.scale), pady=int(2 * self.scale),
            cursor="hand2",
        )
        self.ack_btn.bind("<Button-1>", lambda e: self._acknowledge())
        self.ack_btn.bind("<Button-3>", self._popup_menu)

        # 「选择窗口」：还没指定视频时出现，点一下直接进设置
        self._set_btn_shown = False
        self.set_btn = tk.Label(
            self.inner, text="⚙ 选择窗口", bg="#3a5bd9", fg="#ffffff",
            font=self._font(), padx=int(8 * self.scale), pady=int(2 * self.scale),
            cursor="hand2",
        )
        self.set_btn.bind("<Button-1>", lambda e: self.open_settings())
        self.set_btn.bind("<Button-3>", self._popup_menu)

        # 常驻关闭按钮：一直在最右边，点一下就退出
        self.close_btn = tk.Label(
            self.inner, text="×", bg=BG, fg=self.CLOSE_FG,
            font=self._font(), padx=int(7 * self.scale), pady=int(2 * self.scale),
            cursor="hand2",
        )
        self.close_btn.pack(side="right", fill="y")
        self.close_btn.bind("<Button-1>", lambda e: self.quit())
        self.close_btn.bind("<Button-3>", self._popup_menu)
        self.close_btn.bind("<Enter>", lambda e: self.close_btn.configure(
            fg=self.CLOSE_FG_HOT, bg="#3a1d22"))
        self.close_btn.bind("<Leave>", lambda e: self.close_btn.configure(
            fg=self.CLOSE_FG, bg=BG))

        self._menu = tk.Menu(self.root, tearoff=0,
                             font=("Microsoft YaHei UI", -int(10 * self.scale)))
        self._menu_open = False
        self._topmost_job = None
        self._menu.add_command(label="① 选择要监控的两个窗口…", command=self.open_settings)
        self._menu.add_separator()
        self._sound_var = tk.BooleanVar(value=bool(self.cfg.get("sound", True)))
        self._menu.add_checkbutton(label="提示音", variable=self._sound_var,
                                   command=self._toggle_sound)
        self._top_var = tk.BooleanVar(value=bool(self.cfg.get("topmost", True)))
        self._menu.add_checkbutton(label="始终置顶", variable=self._top_var,
                                   command=self._toggle_topmost)
        self._menu.add_command(label="试听提示音", command=self.alerter.preview)
        self._menu.add_separator()
        self._menu.add_command(label="打开受控浏览器", command=self.launch_browser)
        self._menu.add_command(label="退出", command=self.quit)

        for w in (self.outer, self.inner, *self.labels):
            w.bind("<Button-3>", self._popup_menu)
            w.bind("<Double-Button-1>", lambda e: self.open_settings())

        self.root.bind("<Button-3>", self._popup_menu)
        self._drag = None
        self._pos = [self.cfg["bar"].get("x"), self.cfg["bar"].get("y")]
        if self._pos[0] is None or self._pos[1] is None:
            self._pos = [max(0, (self.root.winfo_screenwidth() - 420) // 2), 40]
        self._pos = [int(self._pos[0]), int(self._pos[1])]
        self._apply_geometry()
        self.root.deiconify()

    def _on_tk_error(self, exc, val, tb):
        log_exception("tk-callback: %s" % getattr(exc, "__name__", exc))
        try:
            messagebox.showerror("出错了", "%s\n\n详情见 %s" % (val, LOG_PATH))
        except Exception:
            pass

    def _bind_drag(self, w):
        w.bind("<Button-1>", self._drag_start)
        w.bind("<B1-Motion>", self._drag_move)
        w.bind("<ButtonRelease-1>", self._drag_end)

    def _drag_start(self, e):
        self._drag = (e.x_root - self._pos[0], e.y_root - self._pos[1])

    def _drag_move(self, e):
        if not self._drag:
            return
        if not (e.state & 0x0100):  # 左键已松开：丢掉这次拖动
            self._drag = None
            return
        x = e.x_root - self._drag[0]
        y = e.y_root - self._drag[1]
        self._pos = [x, y]
        self.root.geometry("%dx%d+%d+%d" % (self._last_w, self._last_h, x, y))

    def _drag_end(self, _e):
        self._drag = None
        self.cfg["bar"]["x"], self.cfg["bar"]["y"] = self._pos
        self.save_config()

    def _popup_menu(self, e):
        """右键菜单：临时取消置顶，否则菜单会被这条置顶栏盖住。"""
        self._menu_open = True
        try:
            self.root.attributes("-topmost", False)
        except tk.TclError:
            pass
        # 菜单放在状态条下方，避免和条本身重叠
        x = min(e.x_root, max(0, self.root.winfo_screenwidth() - int(240 * self.scale)))
        y = self._pos[1] + self._last_h + 1
        try:
            self._menu.tk_popup(x, y)
        finally:
            try:
                self._menu.grab_release()
            except tk.TclError:
                pass
        self._restore_topmost_later()

    def _restore_topmost_later(self):
        if self._topmost_job is not None:
            try:
                self.root.after_cancel(self._topmost_job)
            except Exception:
                pass
        self._topmost_job = self.root.after(200, self._maybe_restore_topmost)

    def _maybe_restore_topmost(self):
        self._topmost_job = None
        try:
            if self._menu.winfo_ismapped():  # 菜单还开着，等它关
                self._topmost_job = self.root.after(250, self._maybe_restore_topmost)
                return
        except tk.TclError:
            pass
        self._menu_open = False
        if self._want_topmost():
            try:
                self.root.attributes("-topmost", True)
                self.root.lift()
            except tk.TclError:
                pass

    # -- 尺寸 ------------------------------------------------------------- #
    _last_w = 200
    _last_h = 30

    def _apply_geometry(self):
        self.root.update_idletasks()
        w = max(self.outer.winfo_reqwidth(), 120)
        h = max(self.outer.winfo_reqheight(), int(22 * self.scale))
        self._last_w, self._last_h = w, h
        x, y = self._clamp(self._pos[0], self._pos[1], w, h)
        self._pos = [x, y]
        self.root.geometry("%dx%d+%d+%d" % (w, h, x, y))

    @staticmethod
    def _clamp(x, y, w, h):
        """把状态条限制在虚拟桌面内，避免换显示器后看不见。"""
        try:
            u = ctypes.windll.user32
            vx, vy = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
            vw, vh = u.GetSystemMetrics(78), u.GetSystemMetrics(79)
        except Exception:
            vx, vy, vw, vh = 0, 0, 1920, 1080
        if vw <= 0 or vh <= 0:
            vx, vy, vw, vh = 0, 0, 1920, 1080
        x = max(vx, min(int(x), vx + vw - w))
        y = max(vy, min(int(y), vy + vh - h))
        return int(x), int(y)

    # -- 定时刷新 --------------------------------------------------------- #
    def _tick(self):
        try:
            snap = None
            try:
                snap = self.q.get_nowait()
            except queue.Empty:
                pass
            if snap is not None:
                self.last_snap = snap
                self._render(snap)
        except Exception:
            log_exception("ui.tick")
        finally:
            try:
                self.root.after(120, self._tick)
            except tk.TclError:
                pass

    def _want_topmost(self) -> bool:
        """右键菜单打开时让一下位置，别把菜单盖住。"""
        return bool(self.cfg.get("topmost", True)) and not self._menu_open

    def _render(self, snap):
        threshold = max(1.0, float(self.cfg.get("threshold_sec", 5) or 5))
        alert_idx = set()
        sizes_changed = False
        for i, ch in enumerate(snap["channels"][:2]):
            lbl = self.labels[i]
            prog = bool(ch.get("progress_hit")) and ch["state"] == STATE_PLAYING
            bad = (ch["state"] in BAD_STATES and ch["bad_sec"] >= threshold) or prog
            if not bad:
                self._acked.discard(i)      # 恢复了：重新武装
            acked = i in self._acked
            if bad and not acked:
                alert_idx.add(i)
            text = self._chan_text(ch, bad, threshold, acked, prog)
            if lbl.cget("text") != text:
                lbl.configure(text=text)
                sizes_changed = True
            if bad and acked:
                # 已经点过「我知道了」：静态不闪，进度提醒用琥珀色区分
                want_bg, want_fg = ("#3a2a12", "#ffcb6b") if prog else ("#4a1a20", "#ff9b9b")
                if lbl.cget("bg") != want_bg or lbl.cget("fg") != want_fg:
                    lbl.configure(bg=want_bg, fg=want_fg)
            elif not bad:
                color = {
                    STATE_PLAYING: C_PLAY,
                    STATE_STOPPED: C_STOP,
                    STATE_BUFFERING: C_WARN,
                    STATE_NOVIDEO: C_WARN,
                    STATE_OFFLINE: C_OFF,
                    STATE_UNBOUND: C_IDLE,
                }.get(ch["state"], C_IDLE)
                if lbl.cget("fg") != color or lbl.cget("bg") != BG:
                    lbl.configure(fg=color, bg=BG)
        self._alert_idx = alert_idx
        self._alerting = bool(alert_idx)
        # 「我知道了」按钮：只在报警时出现
        want_btn = bool(alert_idx)
        if want_btn != self._ack_btn_shown:
            if want_btn:
                self.ack_btn.pack(side="left", fill="y", padx=(int(4 * self.scale), 0))
                self.ack_btn.lift()
            else:
                self.ack_btn.pack_forget()
            self._ack_btn_shown = want_btn
            sizes_changed = True
        # 「选择窗口」按钮：还有通道没指定、或浏览器没连上时出现
        want_set = any(ch["state"] == STATE_UNBOUND for ch in snap["channels"][:2]) \
            or not snap.get("connected")
        if want_set != self._set_btn_shown:
            if want_set:
                self.set_btn.pack(side="left", fill="y", padx=(int(4 * self.scale), 0))
                self.set_btn.lift()
            else:
                self.set_btn.pack_forget()
            self._set_btn_shown = want_set
            sizes_changed = True
        if not self._alerting:
            self.outer.configure(bg=BORDER)
        self.alerter.set_active(self._alerting)
        if sizes_changed:
            self._apply_geometry()
        if self._want_topmost():
            try:
                self.root.attributes("-topmost", True)
            except tk.TclError:
                pass

    def _acknowledge(self):
        """「我知道了」：本次停止闪烁与响铃，文字保留；恢复播放后自动重新武装。"""
        self._acked |= set(self._alert_idx)
        self._alert_idx = set()
        self._alerting = False
        self.alerter.set_active(False)
        self.outer.configure(bg=BORDER)
        if self.last_snap:
            self._render(self.last_snap)

    def _chan_text(self, ch, bad, threshold, acked=False, prog=False):
        label = ch["label"] or "视频"
        st = ch["state"]
        if prog and st == STATE_PLAYING:
            base = "⏩ %s %.0f%% · %s" % (label, ch.get("progress_pct") or 0,
                                          fmt_time(ch["play_time"]))
            if acked:
                base += " · 已静音"
            return base
        if st == STATE_PLAYING:
            extra = " " + fmt_time(ch["play_time"])
        elif st == STATE_STOPPED:
            if bad:
                extra = " %.0fs" % ch["bad_sec"]
            elif "结束" in (ch["note"] or ""):
                extra = "（播放结束）"
            else:
                extra = ""
        elif st == STATE_BUFFERING:
            extra = " %.0fs" % ch["bad_sec"] if bad else ""
        else:
            extra = ""
        base = "%s %s %s%s" % (ICON.get(st, "•"), label, TEXT.get(st, st), extra)
        if st == STATE_UNBOUND:
            base = "… %s 未指定" % label
        if acked and bad:
            base += " · 已静音"
        return base

    def _flash_tick(self):
        try:
            if self._alerting and self._alert_idx:
                self._flash_on = not self._flash_on
                for i in self._alert_idx:
                    self.labels[i].configure(
                        bg=FLASH_A if self._flash_on else FLASH_B,
                        fg="#ffffff" if self._flash_on else "#000000",
                    )
                self.outer.configure(bg=FLASH_A if self._flash_on else FLASH_B)
            else:
                self.outer.configure(bg=BORDER)
            delay = int(max(120, self.cfg.get("flash_ms", 450)))
            self.root.after(delay, self._flash_tick)
        except tk.TclError:
            pass
        except Exception:
            log_exception("ui.flash")

    # -- 配置 ------------------------------------------------------------- #
    def save_config(self):
        try:
            atomic_write_json(CONFIG_PATH, self.cfg)
        except Exception:
            log_exception("save_config")

    def _toggle_sound(self):
        self.cfg["sound"] = bool(self._sound_var.get())
        self.save_config()

    def _toggle_topmost(self):
        self.cfg["topmost"] = bool(self._top_var.get())
        self.root.attributes("-topmost", bool(self.cfg["topmost"]))
        self.save_config()

    def apply_settings(self):
        for w in self.labels + [self.ack_btn, self.set_btn, self.close_btn]:
            w.configure(font=self._font())
        for s in self.seps:
            s.configure(font=("Microsoft YaHei UI", -int(13 * self.scale)))
        try:
            self.root.attributes("-alpha", float(self.cfg.get("opacity", 1.0)))
        except tk.TclError:
            pass
        self.save_config()
        self._apply_geometry()

    # -- 浏览器 ----------------------------------------------------------- #
    def launch_browser(self):
        log_note("launch_browser 开始")
        exe = self.cfg.get("browser_exe") or find_browser()
        if not exe or not os.path.isfile(exe):
            messagebox.showerror("找不到浏览器", "没有找到 Edge 或 Chrome。\n请在设置里手动指定浏览器程序。")
            return
        self.cfg["browser_exe"] = exe
        profile = self.cfg.get("profile_dir") or default_profile_dir()
        self.cfg["profile_dir"] = profile
        os.makedirs(profile, exist_ok=True)
        port = int(self.cfg.get("port", 9222))
        start_url = ""
        page = HELP_PAGE if os.path.isfile(HELP_PAGE) else START_PAGE
        if os.path.isfile(page):
            import pathlib
            start_url = pathlib.Path(page).as_uri()
        args = [
            exe,
            "--remote-debugging-port=%d" % port,
            "--user-data-dir=%s" % profile,
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-session-crashed-bubble",
            "--disable-features=msEdgeSidebarV2",
            start_url or "about:blank",
        ]
        # 用 ShellExecute 而不是 CreateProcess：
        # 打包成 GUI exe 后没有控制台，CreateProcess 继承的 std 句柄是无效的，
        # Edge 会启动后立刻退出；ShellExecute 走的是资源管理器那条路径，稳定。
        params = subprocess.list2cmdline(args[1:])
        try:
            import ctypes
            # 必须显式声明 argtypes：否则 ctypes 会把 Python str 当 char* 传，
            # 指针被截断，轻则调用失败，重则整个进程直接崩掉（没有任何日志）。
            shell32 = ctypes.WinDLL("shell32", use_last_error=True)
            shell32.ShellExecuteW.argtypes = [
                ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p,
                ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int,
            ]
            shell32.ShellExecuteW.restype = ctypes.c_void_p
            rc = shell32.ShellExecuteW(None, "open", exe, params, None, 1) or 0
            log_note("ShellExecute rc=%s exe=%s" % (rc, exe))
            if rc <= 32:
                raise OSError("ShellExecute 返回 %d" % rc)
            # 后台确认端口真的起来了，起不来就明确告诉用户怎么办
            threading.Thread(target=self._verify_browser_started, daemon=True).start()
        except Exception as exc:
            log_exception("launch_browser")
            try:
                subprocess.Popen(
                    args,
                    close_fds=True,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except Exception as exc2:
                messagebox.showerror("启动失败", "无法启动浏览器：\n%s\n%s" % (exc, exc2))
        self.save_config()

    def _verify_browser_started(self):
        """启动浏览器后盯 20 秒端口；没起来就提示用兜底 bat。"""
        port = int(self.cfg.get("port", 9222))
        for _ in range(40):
            time.sleep(0.5)
            try:
                cdp.browser_version(port, 0.6)
                log_note("受控浏览器就绪（端口 %d）" % port)
                return
            except Exception:
                continue
        log_note("受控浏览器 20 秒未就绪")
        try:
            messagebox.showwarning(
                "受控浏览器没起来",
                "已尝试启动浏览器，但 20 秒内没连上调试端口。\n\n"
                "请直接双击程序目录里的「启动受控浏览器.bat」，\n"
                "它会用同样的参数启动 Edge；之后再点「重新连接」。\n\n"
                "详细原因见 %s" % LOG_PATH,
            )
        except Exception:
            pass

    def focus_page(self, target_id: str):
        if not target_id:
            return
        port = int(self.cfg.get("port", 9222))
        threading.Thread(target=lambda: cdp.activate_target(port, target_id), daemon=True).start()

    # -- 设置窗口 --------------------------------------------------------- #
    def open_settings(self):
        if self._settings is not None and self._settings.alive:
            try:
                self._settings.win.deiconify()
                self._settings.win.lift()
                self._settings.win.focus_force()
            except tk.TclError:
                pass
            self._settings.refresh()
            return
        if getattr(self, "_opening", False):
            return
        self._opening = True
        # 不要直接在菜单回调里建窗口：Tk 在这种情况下常常只画出第一块，
        # 交给下一轮事件循环再建，窗口才会完整重绘。
        self.root.after(10, self._create_settings)

    def _create_settings(self):
        self._opening = False
        if not self.root.winfo_exists():
            return
        # 放在状态条下方：不用去动置顶属性，也就不会和"置顶"打架
        w = int(760 * self.scale)
        h = int(720 * self.scale)
        sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        x = min(max(0, self._pos[0]), max(0, sw - w))
        y = self._pos[1] + self._last_h + 10
        if y + h > sh - 40:
            y = max(20, sh - h - 40)
        try:
            self._settings = SettingsWindow(self, x, y)
        except Exception:
            self._settings = None
            log_exception("open_settings")
            messagebox.showerror("无法打开设置",
                                 "设置窗口创建失败，详情已写入\n%s" % LOG_PATH)
            return
        if os.environ.get("TVM_DEBUG_DUMP"):
            self.root.after(2000, self._dump_settings_tree)

    def _dump_settings_tree(self):
        """调试用：把设置窗口里的控件状态写文件。"""
        try:
            s = self._settings
            if s is None:
                return
            lines = ["window %dx%d+%d+%d" % (
                s.win.winfo_width(), s.win.winfo_height(),
                s.win.winfo_rootx(), s.win.winfo_rooty())]
            def walk(w, depth=0):
                for c in w.winfo_children():
                    try:
                        text = c.cget("text")
                    except Exception:
                        text = ""
                    lines.append("%s%s %s mapped=%d %dx%d+%d+%d text=%r" % (
                        "  " * depth, c.winfo_class(), c.winfo_name(),
                        c.winfo_ismapped(), c.winfo_width(), c.winfo_height(),
                        c.winfo_x(), c.winfo_y(), str(text)[:40]))
                    walk(c, depth + 1)
            walk(s.win)
            with open(os.environ["TVM_DEBUG_DUMP"], "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
        except Exception:
            log_exception("dump")

    def quit(self):
        try:
            self.monitor.shutdown()
            self.alerter.shutdown()
        except Exception:
            pass
        self.save_config()
        try:
            self.root.destroy()
        except Exception:
            pass


# ---------------------------------------------------------------- 设置窗口
class SettingsWindow:
    def __init__(self, app: App, x: int = None, y: int = None):
        self.app = app
        self.alive = True
        self.cfg = app.cfg
        self.win = tk.Toplevel(app.root)
        self.win.title("视频播放监控 · 设置")
        self.win.configure(bg="#f4f5f7")
        self.win.attributes("-topmost", True)
        w = int(760 * app.scale)
        h = int(720 * app.scale)
        if x is None:
            x = max(0, (app.root.winfo_screenwidth() - w) // 2)
        if y is None:
            y = 60
        self._x, self._y = int(x), int(y)
        self.win.geometry("%dx%d+%d+%d" % (w, h, self._x, self._y))
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self._font = ("Microsoft YaHei UI", -int(10 * app.scale))
        self._font_b = ("Microsoft YaHei UI", -int(10 * app.scale), "bold")
        self._build()
        self.refresh()
        self.win.after(600, self._auto_tick)
        # 有些情况下（比如刚关掉菜单）新窗口只画出第一块，抖 1 像素强制整窗重绘
        self.win.after(90, self._nudge)

    def _nudge(self):
        if not self.alive:
            return
        try:
            self.win.geometry("+%d+%d" % (self._x, self._y + 1))
            self.win.after(70, self._unnudge)
        except tk.TclError:
            pass

    def _unnudge(self):
        if not self.alive:
            return
        try:
            self.win.geometry("+%d+%d" % (self._x, self._y))
            self.win.lift()
        except tk.TclError:
            pass

    # -- 布局 ------------------------------------------------------------- #
    def _build(self):
        s = self.app.scale
        pad = int(10 * s)
        wrap = tk.Frame(self.win, bg="#f4f5f7")
        wrap.pack(fill="both", expand=True, padx=pad, pady=pad)

        # 浏览器
        box = tk.LabelFrame(wrap, text=" ① 受控浏览器 ", font=self._font_b, bg="#f4f5f7", fg="#333")
        box.pack(fill="x", pady=(0, pad // 2))
        row = tk.Frame(box, bg="#f4f5f7")
        row.pack(fill="x", padx=pad, pady=(4, 2))
        self.lbl_conn = tk.Label(row, text="…", font=self._font, bg="#f4f5f7", fg="#666", anchor="w")
        self.lbl_conn.pack(side="left")
        btns = tk.Frame(box, bg="#f4f5f7")
        btns.pack(fill="x", padx=pad, pady=(2, 6))
        tk.Button(btns, text="启动受控浏览器", font=self._font, command=self._launch).pack(side="left")
        tk.Button(btns, text="重新连接", font=self._font, command=self.refresh).pack(side="left", padx=6)
        tk.Label(box, text="监控必须使用本程序启动的浏览器（独立配置目录，登录一次会记住）。",
                 font=("Microsoft YaHei UI", -int(9 * s)), bg="#f4f5f7", fg="#888", anchor="w").pack(
            fill="x", padx=pad, pady=(0, 4))

        # 窗口列表（先创建、最后 pack，让它吃掉中间的剩余空间）
        box2 = tk.LabelFrame(wrap, text=" ② 选择两个窗口 ", font=self._font_b, bg="#f4f5f7", fg="#333")
        cols = ("win", "title", "url", "video")
        self.tree = ttk.Treeview(box2, columns=cols, show="headings", height=5,
                                 selectmode="browse")
        for c, t, w in (("win", "窗口", 70), ("title", "标签页标题", 240),
                        ("url", "地址", 250), ("video", "视频", 60)):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=int(w * s), anchor="w", stretch=(c != "win"))
        self.tree.pack(fill="both", expand=True, padx=pad, pady=(6, 4))
        self.tree.bind("<Double-Button-1>", lambda e: self._bind(0))

        row2 = tk.Frame(box2, bg="#f4f5f7")
        row2.pack(fill="x", padx=pad, pady=(0, 6))
        tk.Button(row2, text="刷新窗口列表", font=self._font, command=self.refresh).pack(side="left")
        tk.Button(row2, text="自动指定", font=self._font, bg="#dce8ff",
                  command=self._auto_pick).pack(side="left", padx=6)
        tk.Button(row2, text="设为视频1", font=self._font, bg="#d7f5e6",
                  command=lambda: self._bind(0)).pack(side="left")
        tk.Button(row2, text="设为视频2", font=self._font, bg="#ffe9d6",
                  command=lambda: self._bind(1)).pack(side="left", padx=6)
        tk.Button(row2, text="取消绑定", font=self._font,
                  command=self._unbind).pack(side="left")
        self.lbl_bind = tk.Label(box2, text="", font=("Microsoft YaHei UI", -int(9 * s)),
                                 bg="#f4f5f7", fg="#555", anchor="w", justify="left")
        self.lbl_bind.pack(fill="x", padx=pad, pady=(0, 6))

        # 提醒
        box3 = tk.LabelFrame(wrap, text=" ③ 提醒方式 ", font=self._font_b, bg="#f4f5f7", fg="#333")
        box3.pack(side="bottom", fill="x", pady=(0, pad // 2))
        g = tk.Frame(box3, bg="#f4f5f7")
        g.pack(fill="x", padx=pad, pady=6)
        self.v_threshold = tk.IntVar(value=int(self.cfg.get("threshold_sec", 5)))
        self.v_flash = tk.IntVar(value=int(self.cfg.get("flash_ms", 450)))
        self.v_beep = tk.IntVar(value=int(self.cfg.get("beep_interval", 8)))
        self.v_font = tk.IntVar(value=int(self.cfg.get("font_px", 15)))
        self.v_sound = tk.BooleanVar(value=bool(self.cfg.get("sound", True)))
        self.v_opacity = tk.DoubleVar(value=float(self.cfg.get("opacity", 1.0)))
        self.v_auto = tk.BooleanVar(value=self._autostart_enabled())
        self.v_progress = tk.BooleanVar(value=bool(self.cfg.get("progress_alert", True)))
        self.v_progress_pct = tk.IntVar(value=int(self.cfg.get("progress_percent", 90)))
        self.v_progress_chaoxing = tk.BooleanVar(
            value=bool(self.cfg.get("progress_only_chaoxing", True)))

        def num(parent, label, var, lo, hi, r, c, step=1, w=5, unit=""):
            tk.Label(parent, text=label, font=self._font, bg="#f4f5f7").grid(
                row=r, column=c, sticky="e", padx=(0, 4), pady=3)
            tk.Spinbox(parent, from_=lo, to=hi, increment=step, width=w, textvariable=var,
                       font=self._font, justify="center").grid(
                row=r, column=c + 1, sticky="w", pady=3)
            if unit:
                tk.Label(parent, text=unit, font=self._font, bg="#f4f5f7").grid(
                    row=r, column=c + 2, sticky="w", padx=(2, int(18 * s)))

        num(g, "停止多少秒后报警", self.v_threshold, 1, 600, 0, 0, unit="秒")
        num(g, "闪烁间隔", self.v_flash, 120, 3000, 0, 3, step=50, unit="毫秒")
        num(g, "提示音每隔", self.v_beep, 2, 300, 1, 0, unit="秒")
        num(g, "字号", self.v_font, 9, 40, 1, 3, w=4)
        tk.Checkbutton(g, text="声音提醒", variable=self.v_sound, font=self._font,
                       bg="#f4f5f7", activebackground="#f4f5f7").grid(
            row=2, column=3, sticky="w", pady=2)
        tk.Checkbutton(g, text="随 Windows 启动", variable=self.v_auto, font=self._font,
                       bg="#f4f5f7", activebackground="#f4f5f7").grid(
            row=2, column=4, columnspan=2, sticky="w", pady=2)
        tk.Label(g, text="透明度", font=self._font, bg="#f4f5f7").grid(
            row=2, column=0, sticky="e", padx=(0, 4))
        tk.Scale(g, from_=0.35, to=1.0, resolution=0.05, orient="horizontal",
                 variable=self.v_opacity, bg="#f4f5f7", highlightthickness=0,
                 length=int(140 * s), showvalue=True,
                 font=("Microsoft YaHei UI", -int(8 * s))).grid(
            row=2, column=1, columnspan=2, sticky="w", pady=2)
        tk.Button(g, text="试听提示音", font=self._font,
                  command=self.app.alerter.preview).grid(row=1, column=5, columnspan=2,
                                                         sticky="w")
        # 第 4 行：进度提醒（学习通刷课：播到 N% 就叫一声）
        tk.Checkbutton(g, text="播放到", variable=self.v_progress, font=self._font,
                       bg="#f4f5f7", activebackground="#f4f5f7").grid(
            row=3, column=0, sticky="w", pady=(4, 2))
        tk.Spinbox(g, from_=1, to=99, increment=5, width=4, textvariable=self.v_progress_pct,
                   font=self._font, justify="center").grid(
            row=3, column=1, sticky="w", padx=(2, 2), pady=(4, 2))
        tk.Label(g, text="% 时提醒", font=self._font, bg="#f4f5f7").grid(
            row=3, column=2, sticky="w", pady=(4, 2))
        tk.Checkbutton(g, text="只对学习通生效", variable=self.v_progress_chaoxing,
                       font=self._font, bg="#f4f5f7", activebackground="#f4f5f7").grid(
            row=3, column=3, columnspan=3, sticky="w", pady=(4, 2))

        self.lbl_state = tk.Label(wrap, text="", font=("Microsoft YaHei UI", -int(9 * s)),
                                  bg="#f4f5f7", fg="#666", anchor="w", justify="left")
        self.lbl_state.pack(side="bottom", fill="x", pady=(2, 0))

        bottom = tk.Frame(wrap, bg="#f4f5f7")
        bottom.pack(side="bottom", fill="x", pady=(pad // 2, 0))
        tk.Button(bottom, text="保存并关闭", font=self._font_b, width=14,
                  command=self.save_and_close).pack(side="right")
        tk.Button(bottom, text="应用", font=self._font, width=8,
                  command=self.apply).pack(side="right", padx=8)

        # 最后 pack：占满 ①②③ 之间剩下的高度
        box2.pack(side="top", fill="both", expand=True, pady=(0, pad // 2))

    # -- 行为 ------------------------------------------------------------- #
    def _autostart_enabled(self) -> bool:
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                 r"Software\Microsoft\Windows\CurrentVersion\Run")
            try:
                winreg.QueryValueEx(key, APP_NAME)
                return True
            except FileNotFoundError:
                return False
            finally:
                winreg.CloseKey(key)
        except Exception:
            return False

    def _set_autostart(self, on: bool):
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                 r"Software\Microsoft\Windows\CurrentVersion\Run", 0,
                                 winreg.KEY_SET_VALUE)
            try:
                if on:
                    if getattr(sys, "frozen", False):
                        cmdline = '"%s"' % os.path.abspath(sys.executable)
                    else:
                        exe = sys.executable
                        pyw = os.path.join(os.path.dirname(exe), "pythonw.exe")
                        if os.path.isfile(pyw):
                            exe = pyw
                        cmdline = '"%s" "%s"' % (exe, os.path.join(APP_DIR, "main.py"))
                    winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, cmdline)
                else:
                    try:
                        winreg.DeleteValue(key, APP_NAME)
                    except FileNotFoundError:
                        pass
            finally:
                winreg.CloseKey(key)
        except Exception:
            pass

    def _launch(self):
        self.app.launch_browser()
        self.win.after(1500, self.refresh)

    def refresh(self):
        if not self.alive:
            return
        snap = self.app.monitor.snapshot()
        if snap["connected"]:
            txt = "已连接：%s · 端口 %d" % (snap["version"] or "浏览器", int(self.cfg.get("port", 9222)))
            color = "#0a8f4d"
        else:
            txt = "未连接 · 请点击「启动受控浏览器」（端口 %d）" % int(self.cfg.get("port", 9222))
            color = "#c0392b"
        self.lbl_conn.configure(text=txt, fg=color)
        self.lbl_bind.configure(text="当前绑定：" + self._bind_text())
        self.lbl_state.configure(text="正在扫描窗口和视频…")
        # 先用监控线程已经拿到的页面立刻填表，别让用户等空白
        if snap["pages"]:
            self._fill_tree([{"windowId": None, "tabs": snap["pages"]}])
        threading.Thread(target=self._scan, daemon=True).start()

    def _scan(self):
        """后台枚举窗口、分组、统计每个标签页的视频数量。"""
        port = int(self.cfg.get("port", 9222))
        deadline = time.monotonic() + 8.0
        windows = []
        try:
            windows = cdp.probe_windows(port, 3.0)
        except Exception:
            windows = []
        if not windows:
            try:
                pages = cdp.list_pages(port, 2.0)
            except Exception:
                pages = []
            windows = [{"windowId": None, "tabs": pages}] if pages else []
        for g in windows:
            for p in g["tabs"]:
                p["videos"] = "?"
                if not p.get("ws") or time.monotonic() > deadline:
                    continue
                try:
                    conn = cdp.CDPConnection(p["ws"], 2.5)
                    conn.connect()
                    try:
                        p["videos"] = frames.count_videos(FrameProber(conn))
                    finally:
                        conn.close()
                except Exception:
                    p["videos"] = "-"
        self._ui(lambda: self._fill_tree(windows))

    def _fill_tree(self, windows):
        if not self.alive:
            return
        sel = self._selected_page_id()
        for item in self.tree.get_children():
            self.tree.delete(item)
        self._pages = {}
        for wi, g in enumerate(windows, 1):
            for p in g["tabs"]:
                self._pages[p["id"]] = p
                self.tree.insert("", "end", iid=p["id"], values=(
                    "窗口%d" % wi, p["title"], p["url"], p.get("videos", "?")))
        if sel and self.tree.exists(sel):
            self.tree.selection_set(sel)
        self._auto_state()

    def _selected_page_id(self):
        sel = self.tree.selection()
        return sel[0] if sel else None

    def _ui(self, fn):
        try:
            if self.alive:
                self.win.after(0, fn)
        except tk.TclError:
            pass

    def _bind_text(self):
        out = []
        for i, ch in enumerate(self.cfg["channels"][:2]):
            hint = ch.get("title_hint") or ""
            out.append("%s ← %s" % (ch.get("label") or "视频%d" % (i + 1), hint or "（未指定）"))
        return "   ".join(out)

    def _selected_page(self):
        tid = self._selected_page_id()
        if not tid:
            return None
        if tid in getattr(self, "_pages", {}):
            return self._pages[tid]
        for p in self.app.monitor.snapshot()["pages"]:
            if p["id"] == tid:
                return p
        return {"id": tid, "title": self.tree.set(tid, "title"),
                "url": self.tree.set(tid, "url"), "ws": ""}

    def _bind(self, index):
        page = self._selected_page()
        if not page:
            messagebox.showinfo("提示", "请先在上面的列表里选中一个窗口/标签页。", parent=self.win)
            return
        other = 1 - index
        oc = self.cfg["channels"][other]
        if oc.get("url_hint") == page["url"] and page["url"]:
            messagebox.showinfo("提示", "这个窗口已经分配给另一个通道了，请选择不同的窗口。",
                                parent=self.win)
            return
        self.app.monitor.bind(index, page)
        self.cfg["channels"][index]["label"] = "视频%d" % (index + 1)
        self.app.save_config()
        self.lbl_bind.configure(text="当前绑定：" + self._bind_text())
        self._auto_state()

    def _unbind(self):
        for i in range(2):
            self.app.monitor.bind(i, None)
            self.cfg["channels"][i]["url_hint"] = ""
            self.cfg["channels"][i]["title_hint"] = ""
        self.app.save_config()
        self.lbl_bind.configure(text="当前绑定：" + self._bind_text())
        self._auto_state()

    def _auto_pick(self):
        """自动指定：挑两个"有视频"的窗口（优先正在播放的）绑到通道 1 / 2。"""
        self.lbl_state.configure(text="正在自动识别带视频的窗口…")
        threading.Thread(target=self._auto_pick_worker, daemon=True).start()

    def _auto_pick_worker(self):
        port = int(self.cfg.get("port", 9222))
        try:
            windows = cdp.probe_windows(port, 3.0)
        except Exception:
            windows = []
        cands = []  # (score, window_index, page)
        for wi, g in enumerate(windows):
            for p in g["tabs"]:
                if not p.get("ws"):
                    continue
                try:
                    conn = cdp.CDPConnection(p["ws"], 2.5)
                    conn.connect()
                    try:
                        res = FrameProber(conn).probe(1.5)
                    finally:
                        conn.close()
                except Exception:
                    continue
                vids = [
                    v for v in (res or {}).get("vs", [])
                    if (v.get("w") or 0) > 0 or (v.get("rs") or 0) >= 2 or (v.get("d") or 0) > 0
                ]
                if not vids:
                    continue
                playing = any(not v.get("p") and not v.get("e") for v in vids)
                cands.append((len(vids) + (5 if playing else 0), wi, p))
        cands.sort(key=lambda x: -x[0])
        chosen, used_windows = [], set()
        for score, wi, p in cands:      # 先每个窗口挑一个
            if len(chosen) < 2 and wi not in used_windows:
                chosen.append(p)
                used_windows.add(wi)
        for score, wi, p in cands:      # 窗口不够就从剩下的标签页补
            if len(chosen) >= 2:
                break
            if p not in chosen:
                chosen.append(p)
        self._ui(lambda: self._apply_auto(chosen))

    def _apply_auto(self, chosen):
        if not self.alive:
            return
        if len(chosen) < 2:
            messagebox.showinfo(
                "自动指定",
                "只找到 %d 个带视频的窗口。\n\n"
                "请在这个受控浏览器里再开一个窗口放第二个视频，\n"
                "或者在列表里手动选中后点「设为视频2」。" % len(chosen),
                parent=self.win)
        for i, page in enumerate(chosen[:2]):
            self.app.monitor.bind(i, page)
            self.cfg["channels"][i]["label"] = "视频%d" % (i + 1)
        self.app.save_config()
        self.lbl_bind.configure(text="当前绑定：" + self._bind_text())
        self.refresh()

    def _auto_state(self):
        snap = self.app.monitor.snapshot()
        parts = []
        for ch in snap["channels"]:
            parts.append("%s：%s%s" % (
                ch["label"],
                {"playing": "播放中", "stopped": "已停止", "buffering": "卡住了",
                 "novideo": "无视频", "offline": "未连接", "unbound": "未指定"}.get(ch["state"], ch["state"]),
                ("（%.1fs）" % ch["bad_sec"]) if ch["bad_sec"] > 0 else "",
            ))
        self.lbl_state.configure(text="实时状态：" + "    ".join(parts))

    def _auto_tick(self):
        if not self.alive:
            return
        try:
            self._auto_state()
            self.win.after(700, self._auto_tick)
        except tk.TclError:
            self.alive = False

    def apply(self):
        try:
            self.cfg["threshold_sec"] = max(1, int(self.v_threshold.get()))
            self.cfg["flash_ms"] = max(120, int(self.v_flash.get()))
            self.cfg["beep_interval"] = max(2, int(self.v_beep.get()))
            self.cfg["font_px"] = max(9, min(40, int(self.v_font.get())))
            self.cfg["sound"] = bool(self.v_sound.get())
            self.cfg["opacity"] = round(float(self.v_opacity.get()), 2)
            self.cfg["progress_alert"] = bool(self.v_progress.get())
            self.cfg["progress_percent"] = max(1, min(99, int(self.v_progress_pct.get())))
            self.cfg["progress_only_chaoxing"] = bool(self.v_progress_chaoxing.get())
            auto = bool(self.v_auto.get())
            self.cfg["autostart"] = auto
            self._set_autostart(auto)
        except Exception as exc:
            messagebox.showerror("保存失败", str(exc), parent=self.win)
            return
        self.app._sound_var.set(self.cfg["sound"])
        self.app.apply_settings()

    def save_and_close(self):
        self.apply()
        self.close()

    def close(self):
        self.alive = False
        try:
            self.win.destroy()
        except tk.TclError:
            pass
        if self.app._settings is self:
            self.app._settings = None
        try:
            if self.app.cfg.get("topmost", True):
                self.app.root.attributes("-topmost", True)
                self.app.root.lift()
        except tk.TclError:
            pass


# ---------------------------------------------------------------- 入口
def self_test(port: int = 9222):
    """命令行自检：打印每个页面的视频播放状态（不弹窗）。"""
    print("== 探测 127.0.0.1:%d ==" % port)
    try:
        info = cdp.browser_version(port)
        print("浏览器:", info.get("Browser"))
    except Exception as exc:
        print("连接失败:", exc)
        return 1
    try:
        windows = cdp.probe_windows(port, 3.0)
    except Exception as exc:
        windows = []
        print("窗口枚举失败:", exc)
    for wi, g in enumerate(windows, 1):
        print("窗口%d (id=%s)" % (wi, g["windowId"]))
        for p in g["tabs"]:
            n = "-"
            try:
                conn = cdp.CDPConnection(p["ws"], 2.0)
                conn.connect()
                try:
                    res = FrameProber(conn).probe(2.0)
                finally:
                    conn.close()
                n = "%s 个（%s 层 frame）" % (res.get("n"), res.get("frames"))
            except Exception as exc:
                n = "错误:%s" % exc
            print("   -", p["title"][:50], "|", p["url"][:60], "| 视频:", n)
    return 0


def acquire_single_instance() -> bool:
    try:
        handle = ctypes.windll.kernel32.CreateMutexW(None, False, "Local\\TopVideoMonitor_Mutex")
        if ctypes.windll.kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
            return False
        globals()["_MUTEX_HANDLE"] = handle
        return True
    except Exception:
        return True
