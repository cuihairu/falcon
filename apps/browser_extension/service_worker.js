const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  sniffMedia: true,
  sniffIncludeSegments: false,
  sniffMaxItemsPerTab: 80,
  disabledHosts: [],
});

function storageGet(keys) {
  return new Promise((resolve) => chrome.storage.sync.get(keys, resolve));
}

function storageSet(values) {
  return new Promise((resolve) => chrome.storage.sync.set(values, resolve));
}

const sessionStorage = chrome.storage.session || chrome.storage.local;

function sessionGet(keys) {
  return new Promise((resolve) => sessionStorage.get(keys, resolve));
}

function sessionSet(values) {
  return new Promise((resolve) => sessionStorage.set(values, resolve));
}

function sessionRemove(keys) {
  return new Promise((resolve) => sessionStorage.remove(keys, resolve));
}

function downloadsCancel(id) {
  return new Promise((resolve) => chrome.downloads.cancel(id, () => resolve()));
}

function downloadsErase(id) {
  return new Promise((resolve) => chrome.downloads.erase({ id }, () => resolve()));
}

function downloadsDownload(options) {
  return new Promise((resolve) => chrome.downloads.download(options, resolve));
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

function headerValue(headers, name) {
  if (!Array.isArray(headers)) return "";
  const needle = name.toLowerCase();
  const h = headers.find((x) => String(x?.name || "").toLowerCase() === needle);
  return String(h?.value || "");
}

function isLikelySegmentUrl(url) {
  const u = url.toLowerCase();
  return (
    u.includes(".m4s") ||
    u.includes(".cmfv") ||
    u.includes(".cmfa") ||
    u.endsWith(".ts") ||
    u.includes(".ts?") ||
    u.includes("segment") ||
    u.includes("seg-") ||
    u.includes("chunk") ||
    u.includes("frag")
  );
}

function classifyMediaUrl(url, contentType) {
  const u = url.toLowerCase();
  const ct = (contentType || "").toLowerCase();

  const isHls =
    u.includes(".m3u8") ||
    ct.includes("application/vnd.apple.mpegurl") ||
    ct.includes("application/x-mpegurl") ||
    ct.includes("vnd.apple.mpegurl");
  if (isHls) return { kind: "hls", label: "HLS" };

  const isDash = u.includes(".mpd") || ct.includes("application/dash+xml");
  if (isDash) return { kind: "dash", label: "DASH" };

  if (ct.startsWith("video/")) return { kind: "video", label: "Video" };
  if (ct.startsWith("audio/")) return { kind: "audio", label: "Audio" };

  const extMatch = u.match(/\.([a-z0-9]{2,5})(?:$|[?#])/);
  const ext = extMatch ? extMatch[1] : "";
  if (["mp4", "webm", "mkv", "mov", "m4v"].includes(ext)) return { kind: "video", label: "Video" };
  if (["mp3", "m4a", "aac", "flac", "wav", "ogg"].includes(ext)) return { kind: "audio", label: "Audio" };
  return null;
}

function getFilenameFromUrl(url) {
  try {
    const u = new URL(url);
    const base = u.pathname.split("/").filter(Boolean).pop() || "";
    return base;
  } catch {
    return "";
  }
}

function getHostnameSafe(url) {
  try {
    return new URL(url).hostname;
  } catch {
    return "";
  }
}

async function upsertMediaItemForTab(tabId, item, maxItems) {
  const key = `media.tab.${tabId}`;
  const stored = await sessionGet([key]);
  const list = Array.isArray(stored[key]) ? stored[key] : [];
  const idx = list.findIndex((x) => x?.url === item.url);
  if (idx >= 0) {
    list[idx] = { ...list[idx], ...item };
  } else {
    list.unshift(item);
  }
  if (list.length > maxItems) {
    list.length = maxItems;
  }
  await sessionSet({ [key]: list });
}

async function getMediaForTab(tabId) {
  const key = `media.tab.${tabId}`;
  const stored = await sessionGet([key]);
  return Array.isArray(stored[key]) ? stored[key] : [];
}

async function clearMediaForTab(tabId) {
  await sessionRemove([`media.tab.${tabId}`]);
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

chrome.tabs.onRemoved.addListener(async (tabId) => {
  await clearMediaForTab(tabId);
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

chrome.webRequest.onHeadersReceived.addListener(
  async (details) => {
    try {
      if (!details || typeof details.tabId !== "number" || details.tabId < 0) return;
      if (!details.url || details.url.startsWith("blob:")) return;

      const settings = await getSettings();
      if (!settings.sniffMedia) return;

      const host = getHostnameSafe(details.url);
      if (host && isHostDisabled(host, settings.disabledHosts)) return;

      const contentType = headerValue(details.responseHeaders, "content-type");
      const contentLengthRaw = headerValue(details.responseHeaders, "content-length");
      const contentDisposition = headerValue(details.responseHeaders, "content-disposition");

      const classification = classifyMediaUrl(details.url, contentType);
      if (!classification) return;

      if (!settings.sniffIncludeSegments && isLikelySegmentUrl(details.url)) {
        return;
      }

      const size = Number.parseInt(contentLengthRaw || "", 10);
      const filename = getFilenameFromUrl(details.url);

      await upsertMediaItemForTab(
        details.tabId,
        {
          url: details.url,
          kind: classification.kind,
          label: classification.label,
          mime: contentType,
          size: Number.isFinite(size) ? size : null,
          filename,
          disposition: contentDisposition,
          seenAt: Date.now(),
          initiator: details.initiator || "",
        },
        Math.max(10, Math.min(400, settings.sniffMaxItemsPerTab || DEFAULT_SETTINGS.sniffMaxItemsPerTab)),
      );
    } catch {
      // Ignore sniffer errors.
    }
  },
  { urls: ["<all_urls>"] },
  ["responseHeaders", "extraHeaders"],
);

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  (async () => {
    if (msg?.type === "getMediaForTab" && typeof msg.tabId === "number") {
      const list = await getMediaForTab(msg.tabId);
      sendResponse({ ok: true, items: list });
      return;
    }
    if (msg?.type === "clearMediaForTab" && typeof msg.tabId === "number") {
      await clearMediaForTab(msg.tabId);
      sendResponse({ ok: true });
      return;
    }
    if (msg?.type === "sendUrlToFalcon" && typeof msg.url === "string") {
      const settings = await getSettings();
      const ok = await sendToFalcon(settings.apiBaseUrl, {
        url: msg.url,
        referrer: msg.referrer || "",
        filename: msg.filename || "",
        user_agent: msg.userAgent || "",
      });
      if (!ok && settings.launchFalconIfUnavailable) {
        await tryLaunchFalcon(msg.url);
      }
      sendResponse({ ok });
      return;
    }
    if (msg?.type === "downloadInBrowser" && typeof msg.url === "string") {
      const id = await downloadsDownload({ url: msg.url, conflictAction: "uniquify" });
      sendResponse({ ok: typeof id === "number" });
      return;
    }
    if (msg?.type === "skipOnce" && typeof msg.tabId === "number") {
      await setSkipOnceForTab(msg.tabId);
      sendResponse({ ok: true });
      return;
    }
    sendResponse({ ok: false });
  })();
  return true;
});
