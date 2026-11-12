# -*- coding: utf-8 -*-
"""开发期辅助：直接起界面并自动打开设置窗口，方便调布局。

    python devui.py [--no-settings]
"""
import sys

import app

cfg = app.load_config()
a = app.App(cfg)
if "--no-settings" not in sys.argv:
    a.root.after(700, a.open_settings)
a.root.mainloop()
