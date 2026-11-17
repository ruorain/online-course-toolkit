/* 不暂停 — popup 交互 */
'use strict';

const DEFAULTS = { disabledSites: [], autoResume: true, blockFocusLoss: true };

const $ = (id) => document.getElementById(id);
const el = {
  card: $('masterCard'),
  title: $('masterTitle'),
  hint: $('masterHint'),
  siteOn: $('siteOn'),
  dot: $('siteDot'),
  origin: $('siteOrigin'),
  autoResume: $('autoResume'),
  blockFocusLoss: $('blockFocusLoss'),
  warn: $('warn'),
  stat: $('stat')
};

let tab = null;
let site = null;      // 当前页面 origin；null 表示不支持
let supported = false;

/** 从 URL 里取站点标识，和 bridge.js 的 siteKey() 保持一致 */
function siteFromUrl(url) {
  try {
    const u = new URL(url);
    if (u.protocol !== 'http:' && u.protocol !== 'https:') return null;
    return u.origin;
  } catch (_) {
    return null;
  }
}

function sendToTab(msg) {
  return new Promise((resolve) => {
    if (!tab || tab.id == null) return resolve(null);
    try {
      chrome.tabs.sendMessage(tab.id, msg, { frameId: 0 }, (res) => {
        if (chrome.runtime.lastError) return resolve(null);
        resolve(res || null);
      });
    } catch (_) {
      resolve(null);
    }
  });
}

function paintMaster(on) {
  el.siteOn.checked = on;
  el.card.classList.toggle('on', on);
  el.card.classList.toggle('off', !on);
  el.dot.classList.toggle('off', !on);
  if (!supported) {
    el.title.textContent = '当前网站';
    el.hint.textContent = '此页面不支持';
    return;
  }
  el.title.textContent = on ? '已开启 · 本站不暂停' : '已关闭 · 本站照常暂停';
  el.hint.textContent = on ? '移开鼠标、切窗口都不会停' : '仅影响当前网站，其他网站不受影响';
}

function paintStats(state) {
  if (!state) {
    el.stat.textContent = supported ? '等待页面响应…' : '—';
    return;
  }
  const blocked = Number(state.blocked) || 0;
  const resumed = Number(state.resumed) || 0;
  const failed = Number(state.failed) || 0;
  const parts = [`已拦截 ${blocked} 次`];
  if (resumed) parts.push(`已续播 ${resumed} 次`);
  if (failed) parts.push(`续播失败 ${failed} 次`);
  el.stat.textContent = parts.join(' · ');
}

function showWarn(text) {
  if (!text) {
    el.warn.hidden = true;
    el.warn.textContent = '';
    return;
  }
  el.warn.hidden = false;
  el.warn.textContent = text;
}

async function refreshState() {
  if (!supported) return paintStats(null);
  const state = await sendToTab({ type: 'getState' });
  if (!state) {
    showWarn('页面还没加载好（或刚刷新过），刷新一下或稍等再点。');
    paintStats(null);
    return;
  }
  showWarn('');
  paintStats(state);
}

async function init() {
  const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
  tab = tabs && tabs[0] ? tabs[0] : null;
  const url = (tab && tab.url) || '';
  site = siteFromUrl(url);
  supported = !!site;

  el.origin.textContent = site || (url.startsWith('file:') ? '本地文件页面' : '浏览器内部页面');

  const cfg = await chrome.storage.local.get(DEFAULTS);
  const list = Array.isArray(cfg.disabledSites) ? cfg.disabledSites : [];
  const off = site ? list.includes(site) : false;

  el.autoResume.checked = cfg.autoResume !== false;
  el.blockFocusLoss.checked = cfg.blockFocusLoss !== false;
  el.siteOn.disabled = !supported;
  paintMaster(supported && !off);

  if (!supported) {
    showWarn('这是浏览器内部页面（edge:// 或扩展商店），插件没有权限在里面工作。换一个普通网页再试。');
    el.stat.textContent = '—';
    return;
  }

  await refreshState();
}

/* ---------- 交互 ---------- */

el.siteOn.addEventListener('change', async () => {
  if (!supported) return;
  const wantOn = el.siteOn.checked;
  const res = await sendToTab({ type: 'setSiteOff', off: !wantOn });
  if (!res || !res.ok) {
    el.siteOn.checked = !wantOn;      // 回滚
    showWarn('设置失败：页面可能刚跳转或还没注入脚本，刷新页面后重试。');
    return;
  }
  showWarn('');
  paintMaster(wantOn);
});

async function saveOption(key, value) {
  const patch = {};
  patch[key] = value;
  await chrome.storage.local.set(patch);
}

el.autoResume.addEventListener('change', () => saveOption('autoResume', el.autoResume.checked));
el.blockFocusLoss.addEventListener('change', () => saveOption('blockFocusLoss', el.blockFocusLoss.checked));

init().catch((err) => {
  showWarn('初始化失败：' + String(err));
});
