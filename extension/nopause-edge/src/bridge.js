/*!
 * 不暂停 (NoPause) — 隔离世界桥接
 * 负责三件事：
 *   1. 读 chrome.storage，把「本站开不开 / 两个子开关」下发给 MAIN world 的 guard.js；
 *   2. 把 guard 上报的统计缓存起来，供 popup 查询；
 *   3. 处理 popup / 快捷键发来的「开关本站」指令（只有顶层 frame 应答）。
 */
(() => {
  'use strict';

  const FLAG = '__noPauseBridge__';
  if (window[FLAG]) return;
  Object.defineProperty(window, FLAG, { value: 1, enumerable: false, configurable: false });

  const DEFAULTS = { disabledSites: [], autoResume: true, blockFocusLoss: true };
  const isTop = (() => { try { return window.top === window; } catch (_) { return false; } })();

  let cfg = Object.assign({}, DEFAULTS);
  let last = null;

  const post = (msg) => window.postMessage(Object.assign({ __noPause: 1, dir: 'iso2main' }, msg), '*');

  /** 站点标识：用 origin（协议+域名+端口） */
  function siteKey() {
    try {
      const o = location.origin;
      return o && o !== 'null' ? o : null;
    } catch (_) { return null; }
  }

  function effective() {
    const key = siteKey();
    const list = Array.isArray(cfg.disabledSites) ? cfg.disabledSites : [];
    const off = key ? list.includes(key) : false;
    return {
      on: !off,
      off,
      site: key,
      autoResume: cfg.autoResume !== false,
      blockFocusLoss: cfg.blockFocusLoss !== false
    };
  }

  const push = () => post({ type: 'config', payload: effective() });

  async function load() {
    try {
      const got = await chrome.storage.local.get(DEFAULTS);
      cfg = Object.assign({}, DEFAULTS, got);
    } catch (_) { /* 极少数环境下 storage 不可用，用默认值 */ }
    push();
  }

  window.addEventListener('message', (ev) => {
    if (ev.source !== window) return;
    const d = ev.data;
    if (!d || d.__noPause !== 1 || d.dir !== 'main2iso') return;
    if (d.type === 'ready') push();
    else if (d.type === 'state') last = d.payload;
  });

  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== 'local') return;
    for (const k of Object.keys(changes)) cfg[k] = changes[k].newValue;
    push();
  });

  async function setSiteOff(off) {
    const key = siteKey();
    if (!key) return { ok: false, reason: 'unsupported' };
    const got = await chrome.storage.local.get(DEFAULTS);
    const list = new Set(Array.isArray(got.disabledSites) ? got.disabledSites : []);
    if (off) list.add(key); else list.delete(key);
    cfg.disabledSites = Array.from(list);
    await chrome.storage.local.set({ disabledSites: cfg.disabledSites });
    push();
    return { ok: true, off, site: key, ...effective() };
  }

  chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    if (!isTop || !msg || typeof msg !== 'object') return false;

    if (msg.type === 'getState') {
      post({ type: 'ping' });
      setTimeout(() => {
        sendResponse(Object.assign({ ok: true, site: siteKey() }, effective(), last || {}));
      }, 60);
      return true;
    }

    if (msg.type === 'setSiteOff') {
      setSiteOff(!!msg.off).then(sendResponse, (err) => sendResponse({ ok: false, error: String(err) }));
      return true;
    }

    return false;
  });

  load();
})();
