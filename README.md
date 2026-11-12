# TopVideoMonitor · 置顶视频播放监控条

一条永远浮在最上层的一行状态条，实时显示**两个浏览器窗口里的视频是否还在播放**。
一旦某个停止（暂停 / 播完 / 卡住 / 窗口关掉），文字立刻红黄闪烁 + 蜂鸣提醒。

纯 Win32 + Winsock，**零第三方依赖**，编译出来是 ~570 KB 的单文件 exe，内存约 17 MB。

```
● 视频1 播放中 12:34   │   ▲ 视频2 已停止 8s  · 已静音
```

## 特性

- **判定准确**：读 `<video>` 的 `currentTime` 是否在推进，而不是截屏比像素；卡住/缓冲也认得出。
- **穿透 iframe**：学习通、智慧树这类把播放器塞在 2~3 层 iframe 里的站点也能读到（逐 frame 建隔离执行环境）。
- **提醒醒目**：整条边框 + 出问题的通道红黄交替闪烁，同时蜂鸣（间隔可调）。
- **「我知道了」**：报警时出现，点一下停止闪烁和响铃，文字变暗红标「已静音」；视频恢复播放后自动重新武装。
- **双击/右键**：右键菜单可开设置、开关提示音、开关置顶、试听、退出；双击打开设置。
- **记住一切**：位置、绑定、阈值、字号、透明度保存在 `%APPDATA%\TopVideoMonitor\config.ini`；写不进去时自动落到 exe 旁边的 `config.ini`（绿色版）。
- **省资源**：空闲约 1% 单核、17 MB 内存；轮询 0.6 s，frame 树 5 s 刷新一次，跳过 `about:blank` 空 frame。

## 构建

需要 **MinGW-w64 (g++ 16+)**，无需 CMake：

```bat
build.bat
```

产物：`TopVideoMonitor.exe`（主程序）、`probe.exe`（命令行自检）。

## 用法

1. 双击 `TopVideoMonitor.exe`，顶部出现一条 `… 视频1 未指定  … 视频2 未指定  ⚙ 选择窗口`。
2. 右键状态条 → **① 选择要监控的两个窗口…**
   - 若还没开浏览器，点 **启动受控浏览器**（会用独立配置目录启动 Edge/Chrome，登录一次会记住）；
   - 在列表里点 **自动指定**，它会挑出两个带视频的页面；
   - 也可以手动选中一行 → **设为视频1**，再选另一行 → **设为视频2**。
3. 关掉设置窗口即可。

> **为什么必须用「受控浏览器」**：Chrome/Edge 从 136 版起禁止对默认配置目录开调试端口，
> 所以程序用一套独立 profile（`%LOCALAPPDATA%\TopVideoMonitor\browser-profile`）启动浏览器，
> 才能通过 CDP 读到视频的真实播放进度。

### 状态含义

| 显示 | 含义 |
|---|---|
| `● 视频1 播放中 12:34` | 正常播放，数字是播放进度 |
| `▲ 视频1 已停止 8s` | 报警中：闪烁 + 响铃，数字是已停时长 |
| `▲ 视频1 已停止 8s · 已静音` | 点过「我知道了」，不再闪/响 |
| `◐ 视频1 卡住了` | 时间不走：缓冲、卡顿 |
| `○ 视频1 无视频` | 页面里没有 `<video>`（开错标签页了） |
| `✕ 视频1 未连接` | 浏览器/窗口关掉了，或找不到该页面 |

## 工作原理

```
TopVideoMonitor.exe
   ├─ 监控线程：HTTP GET /json/list → 每个页面一条 WebSocket（CDP）
   │     └─ Page.getFrameTree → 逐 frame Page.createIsolatedWorld + Runtime.evaluate
   │           └─ 读 video.currentTime / paused / ended / readyState / duration
   ├─ UI 线程：分层置顶窗口、GDI 双缓冲绘制、闪烁定时器、「我知道了」点击
   └─ 声音线程：condition_variable + Beep()
```

- CDP 客户端是自己写的：RFC6455 最小实现（掩码、分片、ping/pong）+ 迷你 JSON 解析器，不引第三方库。
- 判定规则：`currentTime` 在 0.02~3 s 内推进 → 播放中；`!paused` 但不推进 → 卡住；
  `paused/ended` → 已停止；循环播放回到开头按「推进」处理，不会误报。
- 绑定到 target id 而不是 URL，切换章节导致 URL 变化也不会掉绑定。

## 限制

- 只监控「播放状态」，不监控内容；不联网上报，无任何数据外传。
- 必须在**本程序启动的浏览器**里播放；日常用的那个 Edge 窗口读不到（浏览器安全限制）。
- 播放器若是纯 canvas/WebGL 自绘（不产生 `<video>`），读不到，会显示「无视频」。

## 目录

```
src/            源码（json / ws / cdp / monitor / ui / settings / sound / config / util）
res/app.ico     图标
tools/probe.cpp 命令行自检：打印每个标签页的 frame 数与视频数
build.bat       一键构建
使用说明.html   受控浏览器启动时打开的说明页
```

## 许可

MIT
