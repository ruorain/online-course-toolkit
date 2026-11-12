# -*- coding: utf-8 -*-
"""开发期：复现"设置窗口空白"问题（验证 topmost 切换是否触发不重绘）。"""
import sys

import app

a = app.App(app.load_config())
a.root.update()
if "--drop-topmost" in sys.argv:
    a.root.attributes("-topmost", False)
s = app.SettingsWindow(a)
s.win.geometry("+40+60")
a.root.update()
print("built, topmost dropped:", "--drop-topmost" in sys.argv, flush=True)
a.root.after(6000, a.root.destroy)
a.root.mainloop()
