/*!
 * 不暂停 (NoPause) — 页面主世界守卫
 * 运行在 MAIN world / document_start，比页面自己的脚本更早执行。
 *
 * 它做四件事：
 *   1. 在 window 捕获阶段「吞掉」鼠标离开播放器/窗口、blur、visibilitychange 事件，
 *      让页面注册的暂停处理器根本没机会执行；
 *   2. 把 document.hidden / visibilityState / hasFocus 伪装成「一直在前台」，
 *      对付用轮询而不是事件来判断的播放器；
 *   3. 鼠标离开后的短暂窗口内，让 video.pause() 直接失效（防止站点在别的路径上补刀）；
 *   4. 兜底：万一视频还是被暂停了，立刻 play() 续播 —— 但用户自己点的暂停绝不打扰。
 *
 * 本文件与 bridge.js 通过 window.postMessage 通信（MAIN world 拿不到 chrome.* API）。
 */
(() => {
  'use strict';

  const NS = '__noPauseGuard__';
  if (window[NS]) return;
  Object.defineProperty(window, NS, { value: 1, enumerable: false, configurable: false });

  const W = window;
  const D = document;
  const now = () => performance.now();
  const post = W.postMessage.bind(W);

  /** 生效状态，由 bridge 下发 */
  const state = { on: false, autoResume: true, blockFocusLoss: true };
  const stats = { blocked: 0, resumed: 0, failed: 0 };

  /* ------------------------------------------------------------------ *
   * 1. 伪装「页面可见 / 窗口有焦点」
   * ------------------------------------------------------------------ */
  let descHidden = null;
  let descVis = null;
  let nativeHasFocus = null;
  try { descHidden = Object.getOwnPropertyDescriptor(Document.prototype, 'hidden'); } catch (_) {}
  try { descVis = Object.getOwnPropertyDescriptor(Document.prototype, 'visibilityState'); } catch (_) {}
  try { nativeHasFocus = Document.prototype.hasFocus; } catch (_) {}

  const shield = () => state.on && state.blockFocusLoss;

  try {
    if (descHidden && descHidden.get) {
      Object.defineProperty(D.prototype, 'hidden', {
        configurable: true,
        enumerable: true,
        get() { return shield() ? false : descHidden.get.call(this); }
      });
    }
    if (descVis && descVis.get) {
      Object.defineProperty(D.prototype, 'visibilityState', {
        configurable: true,
        enumerable: true,
        get() { return shield() ? 'visible' : descVis.get.call(this); }
      });
    }
    if (typeof nativeHasFocus === 'function') {
      Object.defineProperty(D.prototype, 'hasFocus', {
        configurable: true,
        writable: true,
        value: function hasFocus() { return shield() ? true : nativeHasFocus.call(this); }
      });
    }
  } catch (_) { /* 伪装失败不致命，后面还有事件层和续播层 */ }

  /** 真实的可见 / 焦点状态（给续播逻辑自己用） */
  const realHidden = () => {
    try { return descHidden && descHidden.get ? descHidden.get.call(D) : false; } catch (_) { return false; }
  };
  const realFocused = () => {
    try { return typeof nativeHasFocus === 'function' ? nativeHasFocus.call(D) : true; } catch (_) { return true; }
  };

  /* ------------------------------------------------------------------ *
   * 2. 鼠标刚离开的那一小段时间里，拦住暂停调用
   * ------------------------------------------------------------------ */
  const mediaProto = W.HTMLMediaElement && W.HTMLMediaElement.prototype;
  const nativePause = mediaProto && typeof mediaProto.pause === 'function' ? mediaProto.pause : null;
  let pauseGuardUntil = 0;

  if (nativePause) {
    mediaProto.pause = function pause() {
      if (state.on && now() < pauseGuardUntil && !this.ended) {
        stats.blocked++;
        report();
        return;
      }
      return nativePause.apply(this, arguments);
    };
  }

  /* ------------------------------------------------------------------ *
   * 3. 认出「播放器」
   * ------------------------------------------------------------------ */
  const MEDIA_SEL = 'video, audio';
  const CONTAINER_SEL = [
    '[class*="player" i]', '[id*="player" i]',
    '[class*="video" i]', '[id*="video" i]',
    '[class*="plyr" i]', '[class*="dplayer" i]', '[class*="artplayer" i]',
    '[class*="video-js" i]', '[class*="jwplayer" i]', '[class*="jw-player" i]',
    '[class*="bpx" i]', '[class*="bilibili" i]',
    '[class*="xgplayer" i]', '[class*="aliplayer" i]', '[class*="ckplayer" i]',
    '[class*="shaka" i]', '[class*="hls" i]',
    '[data-player]', '[aria-label*="player" i]'
  ].join(',');

  const nodeCache = new WeakMap();       // Element -> { ok, at }
  const NEGATIVE_TTL = 2500;

  /** 这个元素本身是不是播放器（或播放器内部的东西） */
  function isPlayerNode(node) {
    if (!node) return false;
    const el = node.nodeType === 1 ? node : node.parentElement;
    if (!el || !(el instanceof Element)) return false;

    const hit = nodeCache.get(el);
    const t = now();
    if (hit && (hit.ok || t - hit.at < NEGATIVE_TTL)) return hit.ok;

    let ok = false;
    try {
      ok = el.matches(MEDIA_SEL) || el.closest(MEDIA_SEL) !== null || el.closest(CONTAINER_SEL) !== null;
    } catch (_) { ok = false; }
    nodeCache.set(el, { ok, at: t });
    return ok;
  }

  /* ------------------------------------------------------------------ *
   * 4. 吞事件
   * ------------------------------------------------------------------ */
  const SWALLOW = ['mouseout', 'mouseleave', 'pointerout', 'pointerleave', 'blur', 'visibilitychange'];
  let pointerInPlayer = false;
  let lastGestureAt = -1e9;

  function eat(e) {
    e.stopImmediatePropagation();
    stats.blocked++;
    report();
  }

  function onOut(e) {
    if (!state.on) return;
    const type = e.type;

    if (type === 'visibilitychange') {
      if (state.blockFocusLoss) eat(e);
      return;
    }

    if (type === 'blur') {
      const t = e.target;
      if (state.blockFocusLoss && (t === W || t === D || t === D.body)) {
        pauseGuardUntil = now() + 700;
        eat(e);
      }
      return;
    }

    // 指针类事件：先判断是不是「离开页面」
    const t = e.target;
    const leavingPage = !e.relatedTarget || t === D || t === W || t === D.documentElement;
    if (leavingPage) {
      pointerInPlayer = false;
      pauseGuardUntil = now() + 600;
      eat(e);
      return;
    }
    if (isPlayerNode(t)) {
      pauseGuardUntil = now() + 600;
      eat(e);
    }
  }

  /** 记录「用户刚刚动过手」——用来区分手动暂停和站点自动暂停 */
  function onGesture(e) {
    if (!state.on) return;
    if (e.type === 'keydown') {
      const t = e.target;
      const tag = t && t.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || (t && t.isContentEditable)) return;
      lastGestureAt = now();
      return;
    }
    if (isPlayerNode(e.target)) lastGestureAt = now();
  }

  /** 跟踪指针是不是停在播放器上 */
  function onHover(e) {
    if (!state.on) return;
    pointerInPlayer = isPlayerNode(e.target);
  }

  /* ------------------------------------------------------------------ *
   * 5. 自动续播兜底
   * ------------------------------------------------------------------ */
  let budgetStart = -1e9;
  let budgetCount = 0;
  let lastResumeAt = -1e9;

  function onPause(e) {
    if (!state.on || !state.autoResume) return;
    const el = e.target;
    if (!el || !(el instanceof HTMLMediaElement)) return;
    if (el.ended || el.error) return;

    const t = now();
    if (t - lastResumeAt < 260) return;            // 冷却，防止死循环
    if (t - lastGestureAt < 2200) return;          // 用户刚点过 / 按过，是手动暂停
    if (pointerInPlayer) return;                   // 鼠标还停在播放器上，多半是用户操作
    if (!state.blockFocusLoss && (realHidden() || !realFocused())) return;

    if (t - budgetStart > 10000) { budgetStart = t; budgetCount = 0; }
    if (budgetCount >= 8) return;                  // 10 秒内最多救 8 次
    budgetCount++;
    lastResumeAt = t;
    stats.resumed++;

    try {
      const p = el.play();
      if (p && typeof p.then === 'function') {
        p.then(null, () => { stats.failed++; report(); });
      }
    } catch (_) {
      stats.failed++;
    }
    report();
  }

  /* ------------------------------------------------------------------ *
   * 6. 与 bridge 通信
   * ------------------------------------------------------------------ */
  let reportTimer = 0;

  function snapshot() {
    return {
      on: state.on,
      autoResume: state.autoResume,
      blockFocusLoss: state.blockFocusLoss,
      blocked: stats.blocked,
      resumed: stats.resumed,
      failed: stats.failed
    };
  }

  function reportNow() {
    reportTimer = 0;
    post({ __noPause: 1, dir: 'main2iso', type: 'state', payload: snapshot() }, '*');
  }

  function report() {
    if (reportTimer) return;
    reportTimer = setTimeout(reportNow, 200);
  }

  function applyConfig(payload) {
    if (!payload) return;
    state.on = payload.on === true;
    state.autoResume = payload.autoResume !== false;
    state.blockFocusLoss = payload.blockFocusLoss !== false;
    if (!state.on) {
      pointerInPlayer = false;
      pauseGuardUntil = 0;
    }
    reportNow();
  }

  W.addEventListener('message', (ev) => {
    if (ev.source !== W) return;
    const d = ev.data;
    if (!d || d.__noPause !== 1 || d.dir !== 'iso2main') return;
    if (d.type === 'config') applyConfig(d.payload);
    else if (d.type === 'ping') reportNow();
  }, false);

  /* ------------------------------------------------------------------ *
   * 7. 注册监听
   * ------------------------------------------------------------------ */
  for (const type of SWALLOW) {
    W.addEventListener(type, onOut, true);
  }
  D.addEventListener('pause', onPause, true);
  W.addEventListener('mouseover', onHover, { capture: true, passive: true });
  W.addEventListener('pointerover', onHover, { capture: true, passive: true });
  for (const type of ['pointerdown', 'mousedown', 'click', 'keydown']) {
    W.addEventListener(type, onGesture, { capture: true, passive: true });
  }

  // 打招呼：bridge 可能比我们早或晚注入，双向握手最稳
  post({ __noPause: 1, dir: 'main2iso', type: 'ready' }, '*');
})();
