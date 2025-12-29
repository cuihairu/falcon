const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  disabledHosts: [],
});

function i18n(key) {
  return chrome.i18n.getMessage(key) || key;
}

function storageGet(keys) {
  return new Promise((resolve) => chrome.storage.sync.get(keys, resolve));
}

function storageSet(values) {
  return new Promise((resolve) => chrome.storage.sync.set(values, resolve));
}

async function getActiveTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  return tab;
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

async function loadSettings() {
  const stored = await storageGet(Object.keys(DEFAULT_SETTINGS));
  return {
    ...DEFAULT_SETTINGS,
    ...stored,
    disabledHosts: Array.isArray(stored.disabledHosts) ? stored.disabledHosts : DEFAULT_SETTINGS.disabledHosts,
  };
}

async function saveSettings(settings) {
  await storageSet(settings);
}

async function main() {
  document.getElementById("title").textContent = i18n("popupTitle");
  document.getElementById("enabledLabel").textContent = i18n("enabledLabel");
  document.getElementById("siteLabel").textContent = i18n("siteLabel");
  document.getElementById("skipOnce").textContent = i18n("skipOnce");
  document.getElementById("openOptions").textContent = i18n("openOptions");

  const statusEl = document.getElementById("status");
  const enabledEl = document.getElementById("enabled");
  const toggleSiteEl = document.getElementById("toggleSite");

  const tab = await getActiveTab();
  const host = getHostname(tab?.url || "");
  document.getElementById("siteHost").textContent = host || "-";

  const settings = await loadSettings();
  enabledEl.checked = !!settings.enabled;

  const disabled = host ? isHostDisabled(host, settings.disabledHosts) : false;
  toggleSiteEl.textContent = disabled ? i18n("enableSite") : i18n("disableSite");
  toggleSiteEl.disabled = !host;

  enabledEl.addEventListener("change", async () => {
    const next = { ...settings, enabled: enabledEl.checked };
    await saveSettings(next);
    statusEl.textContent = i18n("saved");
  });

  toggleSiteEl.addEventListener("click", async () => {
    if (!host) return;
    const list = Array.isArray(settings.disabledHosts) ? [...settings.disabledHosts] : [];
    const idx = list.findIndex((h) => normalizeHost(h) === normalizeHost(host));
    if (idx >= 0) {
      list.splice(idx, 1);
    } else {
      list.push(host);
    }
    const next = { ...settings, disabledHosts: list };
    await saveSettings(next);
    window.close();
  });

  document.getElementById("skipOnce").addEventListener("click", async () => {
    if (typeof tab?.id !== "number") return;
    await chrome.runtime.sendMessage({ type: "skipOnce", tabId: tab.id });
    statusEl.textContent = i18n("saved");
  });

  document.getElementById("openOptions").addEventListener("click", async () => {
    await chrome.runtime.openOptionsPage();
  });
}

main();

