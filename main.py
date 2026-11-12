# -*- coding: utf-8 -*-
"""入口：置顶视频播放监控条。

用法：
    pythonw main.py            启动（无控制台窗口）
    python  main.py --probe    命令行自检，打印浏览器里每个标签页的视频数量
"""

import argparse
import ctypes
import os
import sys
import traceback

APP_TITLE = "视频播放监控条"


def fatal(msg: str):
    """pythonw 没有控制台，出错时用系统弹窗 + 日志告知。"""
    try:
        import app
        app.log_exception("main")
    except Exception:
        pass
    try:
        ctypes.windll.user32.MessageBoxW(None, str(msg)[:2000], APP_TITLE, 0x10)
    except Exception:
        print(msg, file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=APP_TITLE)
    parser.add_argument("--probe", action="store_true",
                        help="命令行自检：打印浏览器窗口/标签页/视频数量")
    parser.add_argument("--port", type=int, default=0, help="调试端口，默认读配置（9222）")
    parser.add_argument("--debug", action="store_true", help="显示控制台输出")
    args = parser.parse_args()

    import app  # 延迟导入，保证 DPI 设置在创建窗口之前

    cfg = app.load_config()
    if args.port:
        cfg["port"] = args.port

    if args.probe:
        return app.self_test(int(cfg.get("port", 9222)))

    if not app.acquire_single_instance():
        ctypes.windll.user32.MessageBoxW(
            None, "视频播放监控条已经在运行了。\n\n（可在顶部状态条上右键找到设置）",
            APP_TITLE, 0x40)
        return 0

    instance = app.App(cfg)
    try:
        instance.root.mainloop()
    finally:
        try:
            instance.monitor.shutdown()
            instance.alerter.shutdown()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception:
        if "--debug" in sys.argv:
            traceback.print_exc()
        fatal("启动失败：\n" + traceback.format_exc(limit=3))
        sys.exit(1)
