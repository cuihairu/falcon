const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  disabledHosts: [],
});

function storageGet(keys) {
  return new Promise((resolve) => chrome.storage.sync.get(keys, resolve));
}

function storageSet(values) {
  return new Promise((resolve) => chrome.storage.sync.set(values, resolve));
}

function downloadsCancel(id) {
  return new Promise((resolve) => chrome.downloads.cancel(id, () => resolve()));
}

function downloadsErase(id) {
  return new Promise((resolve) => chrome.downloads.erase({ id }, () => resolve()));
}

function notificationsCreate(options) {
  return new Promise((resolve) => chrome.notifications.create(options, resolve));
}

function tabsCreate(createProperties) {
  return new Promise((resolve) => chrome.tabs.create(createProperties, resolve));
}

function tabsRemove(tabId) {
  return new Promise((resolve) => chrome.tabs.remove(tabId, () => resolve()));
}

function getHostname(url) {
  try {
    return new URL(url).hostname;
  } catch {
    return "";
  }
}

function normalizeHost(host) {
  return host.trim().toLowerCase();
}

function hostMatches(host, rule) {
  const h = normalizeHost(host);
  const r = normalizeHost(rule);
  if (!h || !r) return false;
  if (h === r) return true;
  return h.endsWith("." + r);
}

function isHostDisabled(host, disabledHosts) {
  return disabledHosts.some((rule) => hostMatches(host, rule));
}

async function getSettings() {
  const stored = await storageGet(Object.keys(DEFAULT_SETTINGS));
  return {
    ...DEFAULT_SETTINGS,
    ...stored,
    disabledHosts: Array.isArray(stored.disabledHosts) ? stored.disabledHosts : DEFAULT_SETTINGS.disabledHosts,
  };
}

async function setSkipOnceForTab(tabId) {
  const key = `skipOnce.tab.${tabId}`;
  await storageSet({ [key]: Date.now() });
}

async function consumeSkipOnceForTab(tabId) {
  const key = `skipOnce.tab.${tabId}`;
  const stored = await storageGet([key]);
  if (!stored[key]) return false;
  await storageSet({ [key]: null });
  return true;
}

async function sendToFalcon(apiBaseUrl, payload) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 1500);
  try {
    const res = await fetch(`${apiBaseUrl.replace(/\/+$/, "")}/v1/add`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: controller.signal,
    });
    return res.ok;
  } catch {
    return false;
  } finally {
    clearTimeout(timeout);
  }
}

async function notify(title, message) {
  await notificationsCreate({
    type: "basic",
    iconUrl: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMB/6X4pJkAAAAASUVORK5CYII=",
    title,
    message,
  });
}

async function tryLaunchFalcon(url) {
  const deeplink = `falcon://add?url=${encodeURIComponent(url)}`;
  try {
    const tab = await tabsCreate({ url: deeplink, active: false });
    if (tab?.id) {
      setTimeout(() => tabsRemove(tab.id), 1200);
    }
  } catch {
    // Ignore
  }
}

chrome.runtime.onInstalled.addListener(async () => {
  const current = await getSettings();
  await storageSet(current);
});

chrome.downloads.onCreated.addListener(async (item) => {
  if (!item || !item.url || typeof item.id !== "number") return;
  if (item.state === "complete") return;

  const settings = await getSettings();
  if (!settings.enabled) return;

  const host = getHostname(item.url);
  if (host && isHostDisabled(host, settings.disabledHosts)) return;

  if (typeof item.tabId === "number" && item.tabId >= 0) {
    const skip = await consumeSkipOnceForTab(item.tabId);
    if (skip) return;
  }

  const ok = await sendToFalcon(settings.apiBaseUrl, {
    url: item.url,
    referrer: item.referrer || "",
    filename: item.filename || "",
  });

  if (!ok) {
    if (settings.launchFalconIfUnavailable) {
      await tryLaunchFalcon(item.url);
    }
    await notify("Falcon", "Falcon API not reachable; using browser download.");
    return;
  }

  await downloadsCancel(item.id);
  await downloadsErase(item.id);
  await notify("Falcon", "Sent download to Falcon.");
});

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  (async () => {
    if (msg?.type === "skipOnce" && typeof msg.tabId === "number") {
      await setSkipOnceForTab(msg.tabId);
      sendResponse({ ok: true });
      return;
    }
    sendResponse({ ok: false });
  })();
  return true;
});
