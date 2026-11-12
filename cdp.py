# -*- coding: utf-8 -*-
"""极简 Chrome DevTools Protocol 客户端（只用 Python 标准库，零第三方依赖）。

用途：连上由本程序用 --remote-debugging-port 启动的 Edge/Chrome，
      枚举窗口/标签页、在页面里执行 JS、把窗口提到前台。

只实现了本应用需要的一小部分：HTTP 端点 + 一条 WebSocket 上的 JSON-RPC。
"""

from __future__ import annotations

import base64
import json
import os
import socket
import struct
import threading
import time
import urllib.request

# 本机请求必须绕过系统代理，否则设了 HTTP_PROXY 的机器会连不上 127.0.0.1
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class CDPError(Exception):
    """CDP 通讯错误；调用方一般据此判定"浏览器已断开"。"""


# --------------------------------------------------------------------------- #
# HTTP 端点
# --------------------------------------------------------------------------- #
def http_json(port: int, path: str, timeout: float = 1.5):
    url = "http://127.0.0.1:%d%s" % (port, path)
    req = urllib.request.Request(url, headers={"Host": "127.0.0.1:%d" % port})
    try:
        with _OPENER.open(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8", "replace"))
    except Exception as exc:  # URLError / 超时 / JSON 错误
        raise CDPError("HTTP %s 失败: %s" % (path, exc))


def browser_version(port: int, timeout: float = 1.5) -> dict:
    """返回 {'Browser': 'Edge/157...', 'webSocketDebuggerUrl': 'ws://...'}"""
    return http_json(port, "/json/version", timeout)


def list_pages(port: int, timeout: float = 1.5) -> list:
    """返回所有 page 类型的 target：id / title / url / ws。"""
    raw = http_json(port, "/json/list", timeout)
    pages = []
    for t in raw:
        if t.get("type") != "page":
            continue
        pages.append(
            {
                "id": t.get("id", ""),
                "title": t.get("title", "") or "(无标题)",
                "url": t.get("url", ""),
                "ws": t.get("webSocketDebuggerUrl", ""),
            }
        )
    return pages


# --------------------------------------------------------------------------- #
# WebSocket（RFC 6455 的最小实现，够 CDP 用）
# --------------------------------------------------------------------------- #
class WebSocket:
    def __init__(self, url: str, timeout: float = 4.0):
        if not url.startswith("ws://"):
            raise CDPError("只支持 ws:// 地址: %s" % url)
        rest = url[5:]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.host = host
        self.port = int(port or 80)
        self.path = "/" + path
        self.timeout = timeout
        self.sock = None
        self._buf = b""
        self._send_lock = threading.Lock()

    # -- 内部 ------------------------------------------------------------- #
    def _recv_more(self, timeout: float) -> bool:
        self.sock.settimeout(max(0.01, timeout))
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return False
        except OSError as exc:
            raise CDPError("读取失败: %s" % exc)
        if not chunk:
            raise CDPError("连接已被对端关闭")
        self._buf += chunk
        return True

    def _read(self, n: int, deadline: float) -> bytes:
        while len(self._buf) < n:
            left = deadline - time.monotonic()
            if left <= 0:
                raise CDPError("读取超时")
            if not self._recv_more(left):
                if time.monotonic() >= deadline:
                    raise CDPError("读取超时")
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _send_frame(self, opcode: int, payload: bytes):
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        header = bytearray([0x80 | opcode])
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        header += mask
        with self._send_lock:
            self.sock.sendall(bytes(header) + masked)

    # -- 对外 ------------------------------------------------------------- #
    def connect(self):
        try:
            sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as exc:
            raise CDPError("无法连接 %s:%d (%s)" % (self.host, self.port, exc))
        sock.settimeout(self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ) % (self.path, self.host, self.port, key)
        sock.sendall(req.encode())
        self.sock = sock
        head = b""
        deadline = time.monotonic() + self.timeout
        while b"\r\n\r\n" not in head:
            if time.monotonic() > deadline:
                raise CDPError("握手超时")
            chunk = sock.recv(4096)
            if not chunk:
                raise CDPError("握手失败：连接被关闭")
            head += chunk
        header, _, rest = head.partition(b"\r\n\r\n")
        first = header.split(b"\r\n", 1)[0].decode("latin1", "replace")
        if "101" not in first:
            raise CDPError("握手被拒绝: %s" % first)
        self._buf = rest

    def send_text(self, text: str):
        if self.sock is None:
            raise CDPError("连接未建立")
        self._send_frame(0x1, text.encode("utf-8"))

    def recv_text(self, timeout: float):
        """读一条完整文本消息；超时返回 None；对端关闭抛 CDPError。"""
        deadline = time.monotonic() + timeout
        fragments = []
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            hdr = self._read(2, deadline)
            b1, b2 = hdr[0], hdr[1]
            fin = b1 & 0x80
            opcode = b1 & 0x0F
            masked = b2 & 0x80
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._read(2, deadline))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._read(8, deadline))[0]
            mask = self._read(4, deadline) if masked else None
            payload = self._read(length, deadline) if length else b""
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x9:  # ping
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode == 0x8:  # close
                raise CDPError("对端关闭了连接")
            if fin:
                fragments.append(payload)
                data = b"".join(fragments)
                fragments = []
                return data.decode("utf-8", "replace")
            fragments.append(payload)

    def close(self):
        if self.sock is not None:
            try:
                self._send_frame(0x8, b"")
            except Exception:
                pass
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None


# --------------------------------------------------------------------------- #
# CDP 连接（一条 WS 对应一个 target 或 browser）
# --------------------------------------------------------------------------- #
class CDPConnection:
    """对某个 target（页面或浏览器本身）的 JSON-RPC 连接。

    非线程安全：一个连接同一时间只应由一个线程使用。
    """

    def __init__(self, ws_url: str, timeout: float = 3.0):
        self.ws = WebSocket(ws_url, timeout)
        self._id = 0
        self.closed = False
        self.on_event = None  # 收到 CDP 事件时的回调（事件 dict）

    # -- 生命周期 --------------------------------------------------------- #
    def connect(self):
        self.ws.connect()
        self.closed = False

    def close(self):
        self.closed = True
        self.ws.close()

    # -- RPC -------------------------------------------------------------- #
    def call(self, method: str, params: dict = None, timeout: float = 3.0,
             session_id: str = None) -> dict:
        if self.closed:
            raise CDPError("连接已关闭")
        self._id += 1
        mid = self._id
        msg = {"id": mid, "method": method, "params": params or {}}
        if session_id:
            msg["sessionId"] = session_id
        try:
            self.ws.send_text(json.dumps(msg, ensure_ascii=False))
        except OSError as exc:
            self.closed = True
            raise CDPError("发送失败: %s" % exc)
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise CDPError("%s 超时" % method)
            text = self.ws.recv_text(left)
            if text is None:
                continue
            try:
                data = json.loads(text)
            except ValueError:
                continue
            if data.get("method") == "Inspector.detached":
                self.closed = True
                raise CDPError("页面连接已断开")
            if data.get("id") == mid:
                if "error" in data:
                    err = data["error"] or {}
                    raise CDPError("%s: %s" % (method, err.get("message", "未知错误")))
                return data.get("result") or {}
            if data.get("method") and self.on_event is not None:
                try:
                    self.on_event(data)
                except Exception:
                    pass
            # 其它事件（如 Runtime.consoleAPICalled）忽略

    def eval_js(self, expression: str, timeout: float = 3.0,
                context_id: int = None, session_id: str = None):
        params = {
            "expression": expression,
            "returnByValue": True,
            "awaitPromise": False,
            "userGesture": False,
            "timeout": int(timeout * 1000) - 200,
        }
        if context_id is not None:
            params["contextId"] = context_id
        result = self.call("Runtime.evaluate", params, timeout, session_id)
        if result.get("exceptionDetails"):
            detail = result["exceptionDetails"]
            raise CDPError("页面脚本报错: %s" % detail.get("text", "exception"))
        return (result.get("result") or {}).get("value")


# --------------------------------------------------------------------------- #
# 窗口分组 / 窗口操作
# --------------------------------------------------------------------------- #
def probe_windows(port: int, timeout: float = 2.5) -> list:
    """返回 [{windowId, active_title, tabs:[{id,title,url,ws}]}]。

    同一个浏览器窗口里的多个标签页会被归到一组，方便让用户"选窗口"。
    """
    pages = list_pages(port, timeout)
    info = browser_version(port, timeout)
    ws_url = info.get("webSocketDebuggerUrl", "")
    result = []
    conn = None
    try:
        if ws_url:
            conn = CDPConnection(ws_url, timeout)
            conn.connect()
        groups = {}
        for p in pages:
            wid = None
            if conn is not None:
                try:
                    r = conn.call("Browser.getWindowForTarget", {"targetId": p["id"]}, timeout)
                    wid = r.get("windowId")
                except Exception:
                    wid = None
            key = wid if wid is not None else "single-%s" % p["id"]
            g = groups.setdefault(key, {"windowId": wid, "tabs": []})
            g["tabs"].append(p)
        result = list(groups.values())
    finally:
        if conn is not None:
            conn.close()
    return result


def activate_target(port: int, target_id: str, timeout: float = 2.0) -> bool:
    """把某个标签页切到前台（同窗口内激活）。"""
    try:
        info = browser_version(port, timeout)
        conn = CDPConnection(info["webSocketDebuggerUrl"], timeout)
        conn.connect()
        try:
            conn.call("Target.activateTarget", {"targetId": target_id}, timeout)
        finally:
            conn.close()
        return True
    except Exception:
        return False


def restore_window(port: int, window_id: int, timeout: float = 2.0) -> bool:
    """把最小化的浏览器窗口恢复出来。"""
    if window_id is None:
        return False
    try:
        info = browser_version(port, timeout)
        conn = CDPConnection(info["webSocketDebuggerUrl"], timeout)
        conn.connect()
        try:
            conn.call(
                "Browser.setWindowBounds",
                {"windowId": int(window_id), "bounds": {"windowState": "normal"}},
                timeout,
            )
        finally:
            conn.close()
        return True
    except Exception:
        return False
