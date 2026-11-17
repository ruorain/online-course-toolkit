/*!
 * 不暂停 (NoPause) — 后台 service worker
 * 只干两件事：初始化默认设置、维护工具栏角标（ON / OFF）+ 快捷键开关。
 * 所有状态判断都交给页面里的 content script，避免申请 tabs 权限。
 */
'use strict';

const DEFAULTS = { disabledSites: [], autoResume: true, blockFocusLoss: true };

chrome.runtime.onInstalled.addListener(async () => {
  const cur = await chrome.storage.local.get(DEFAULTS);
  await chrome.storage.local.set(cur);
});

/** 问页面要状态，用来画角标 */
async function refreshBadge(tabId) {
  if (typeof tabId !== 'number' || tabId < 0) return;
  try {
    const res = await chrome.tabs.sendMessage(tabId, { type: 'getState' }, { frameId: 0 });
    const on = !!(res && res.on);
    await chrome.action.setBadgeText({ tabId, text: on ? 'ON' : 'OFF' });
    await chrome.action.setBadgeBackgroundColor({ tabId, color: on ? '#16a34a' : '#6b7280' });
  } catch (_) {
    // 页面没有 content script（浏览器内部页 / 应用商店等）
    try { await chrome.action.setBadgeText({ tabId, text: '' }); } catch (e) {}
  }
}

chrome.tabs.onActivated.addListener(({ tabId }) => refreshBadge(tabId));
chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (info.status === 'complete' || info.url) refreshBadge(tabId);
});

chrome.storage.onChanged.addListener(async (changes, area) => {
  if (area !== 'local') return;
  const tabs = await chrome.tabs.query({});
  for (const t of tabs) if (t.id != null) refreshBadge(t.id);
});

chrome.commands.onCommand.addListener(async (command) => {
  if (command !== 'toggle-site') return;
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab || tab.id == null) return;
  try {
    const state = await chrome.tabs.sendMessage(tab.id, { type: 'getState' }, { frameId: 0 });
    if (!state || state.site == null) return;
    await chrome.tabs.sendMessage(tab.id, { type: 'setSiteOff', off: !state.off }, { frameId: 0 });
  } catch (_) { /* 不支持的页面，忽略 */ }
  refreshBadge(tab.id);
});
