const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  sniffMedia: true,
  sniffIncludeSegments: false,
  sniffMaxItemsPerTab: 80,
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
  document.getElementById("snifferTitle").textContent = i18n("snifferTitle");
  document.getElementById("snifferClear").textContent = i18n("snifferClear");

  const statusEl = document.getElementById("status");
  const enabledEl = document.getElementById("enabled");
  const toggleSiteEl = document.getElementById("toggleSite");
  const snifferEmptyEl = document.getElementById("snifferEmpty");
  const mediaListEl = document.getElementById("mediaList");

  const tab = await getActiveTab();
  const host = getHostname(tab?.url || "");
  document.getElementById("siteHost").textContent = host || "-";

  const settings = await loadSettings();
  enabledEl.checked = !!settings.enabled;

  const disabled = host ? isHostDisabled(host, settings.disabledHosts) : false;
  toggleSiteEl.textContent = disabled ? i18n("enableSite") : i18n("disableSite");
  toggleSiteEl.disabled = !host;

  async function refreshMediaList() {
    mediaListEl.replaceChildren();
    snifferEmptyEl.textContent = "";

    if (typeof tab?.id !== "number") {
      snifferEmptyEl.textContent = i18n("snifferEmpty");
      return;
    }

    const res = await chrome.runtime.sendMessage({ type: "getMediaForTab", tabId: tab.id });
    const items = Array.isArray(res?.items) ? res.items : [];
    if (items.length === 0) {
      snifferEmptyEl.textContent = i18n("snifferEmpty");
      return;
    }

    const shown = items.slice(0, 20);
    for (const item of shown) {
      const container = document.createElement("div");
      container.className = "mediaItem";

      const title = document.createElement("div");
      title.className = "mediaTitle";
      const name = item.filename || item.label || "";
      title.textContent = `${item.label || "Media"}${name ? ` — ${name}` : ""}`;

      const meta = document.createElement("div");
      meta.className = "mediaMeta";
      const uHost = getHostname(item.url || "");
      const size = typeof item.size === "number" ? `${Math.round(item.size / 1024)} KB` : "";
      meta.textContent = [uHost, item.mime || item.kind, size].filter(Boolean).join(" · ");

      const url = document.createElement("div");
      url.className = "mediaUrl";
      url.title = item.url || "";
      url.textContent = item.url || "";

      const actions = document.createElement("div");
      actions.className = "mediaActions";

      const btnSend = document.createElement("button");
      btnSend.textContent = i18n("sendToFalcon");
      btnSend.addEventListener("click", async () => {
        statusEl.textContent = "";
        btnSend.disabled = true;
        const ua = navigator.userAgent || "";
        const ok = await chrome.runtime.sendMessage({
          type: "sendUrlToFalcon",
          url: item.url,
          referrer: item.requestReferrer || tab.url || "",
          filename: item.filename || "",
          userAgent: item.requestUserAgent || ua,
        });
        statusEl.textContent = ok?.ok ? i18n("done") : i18n("failed");
        btnSend.disabled = false;
      });

      const btnDownload = document.createElement("button");
      btnDownload.textContent = i18n("downloadInBrowser");
      btnDownload.addEventListener("click", async () => {
        statusEl.textContent = "";
        btnDownload.disabled = true;
        const ok = await chrome.runtime.sendMessage({ type: "downloadInBrowser", url: item.url });
        statusEl.textContent = ok?.ok ? i18n("done") : i18n("failed");
        btnDownload.disabled = false;
      });

      const btnCopy = document.createElement("button");
      btnCopy.textContent = i18n("copyUrl");
      btnCopy.addEventListener("click", async () => {
        try {
          await navigator.clipboard.writeText(item.url || "");
          statusEl.textContent = i18n("done");
        } catch {
          statusEl.textContent = i18n("failed");
        }
      });

      actions.append(btnSend, btnDownload, btnCopy);
      container.append(title, meta, url, actions);
      mediaListEl.appendChild(container);
    }
  }

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

  document.getElementById("snifferClear").addEventListener("click", async () => {
    if (typeof tab?.id !== "number") return;
    await chrome.runtime.sendMessage({ type: "clearMediaForTab", tabId: tab.id });
    await refreshMediaList();
  });

  document.getElementById("openOptions").addEventListener("click", async () => {
    await chrome.runtime.openOptionsPage();
  });

  await refreshMediaList();
}

main();
