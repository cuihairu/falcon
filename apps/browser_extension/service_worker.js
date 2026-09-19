const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  sniffMedia: true,
  sniffIncludeSegments: false,
  sniffMaxItemsPerTab: 80,
  includeCookiesOnSend: false,
  disabledHosts: [],
  // "desktop" = Falcon 桌面 IPC（/v1/add），"daemon" = aria2 兼容 JSON-RPC
  sendTarget: "desktop",
  daemonUrl: "http://127.0.0.1:6800",
  daemonSecret: "",
  // 接管浏览器下载的文件类型（小写扩展名数组）；空数组 = 全部接管
  interceptExtensions: [],
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

function rpcTimeoutSignal(ms) {
  const controller = new AbortController();
  setTimeout(() => controller.abort(), ms);
  return controller.signal;
}

/** aria2 兼容 JSON-RPC 调用（token 前缀认证，daemon /jsonrpc 端点） */
async function daemonRpc(daemonUrl, secret, method, params = []) {
  const res = await fetch(`${daemonUrl.replace(/\/+$/, "")}/jsonrpc`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: Date.now(),
      method,
      params: secret ? [`token:${secret}`, ...params] : params,
    }),
    signal: rpcTimeoutSignal(3000),
  });
  const data = await res.json();
  if (data && data.error) {
    throw new Error(data.error.message || String(data.error.code));
  }
  return data ? data.result : null;
}

/** 发送到 Falcon daemon（aria2.addUri），返回是否成功 */
async function sendToDaemon(settings, payload) {
  const options = {};
  if (payload.filename) options.out = payload.filename;
  if (payload.referrer) options.referer = payload.referrer;
  if (payload.user_agent) options["user-agent"] = payload.user_agent;
  if (payload.cookies) options.header = `Cookie: ${payload.cookies}`;
  const gid = await daemonRpc(settings.daemonUrl, settings.daemonSecret, "aria2.addUri", [
    [payload.url],
    options,
  ]);
  return typeof gid === "string" && gid.length > 0;
}

/** 按发送目标设置分发（桌面 IPC 或 daemon RPC） */
async function sendDownload(settings, payload) {
  if (settings.sendTarget === "daemon") {
    return sendToDaemon(settings, payload);
  }
  return sendToFalcon(settings.apiBaseUrl, payload);
}

/** 归一化 daemon 任务（tellActive/tellWaiting/tellStopped 条目）为统一形状 */
function normalizeDaemonTask(g) {
  const file = Array.isArray(g && g.files) ? g.files[0] : null;
  const total = Number((g && g.totalLength) || 0);
  const done = Number((g && g.completedLength) || 0);
  return {
    id: (g && g.gid) || "",
    url: (file && file.uris && file.uris[0] && file.uris[0].uri) || "",
    path: (file && file.path) || "",
    status: (g && g.status) || "",
    progress: total > 0 ? done / total : 0,
    totalBytes: total,
    downloadedBytes: done,
    speed: Number((g && g.downloadSpeed) || 0),
    error: (g && g.errorMessage) || "",
  };
}

/** 任务列表：desktop 走 /v1/tasks，daemon 走 tellActive+tellWaiting+tellStopped */
async function getTaskList(settings) {
  if (settings.sendTarget === "daemon") {
    const [active, waiting, stopped] = await Promise.all([
      daemonRpc(settings.daemonUrl, settings.daemonSecret, "aria2.tellActive"),
      daemonRpc(settings.daemonUrl, settings.daemonSecret, "aria2.tellWaiting", [0, 100]),
      daemonRpc(settings.daemonUrl, settings.daemonSecret, "aria2.tellStopped", [0, 100]),
    ]);
    return [
      ...(Array.isArray(active) ? active : []),
      ...(Array.isArray(waiting) ? waiting : []),
      ...(Array.isArray(stopped) ? stopped : []),
    ].map(normalizeDaemonTask);
  }
  const res = await fetch(`${settings.apiBaseUrl.replace(/\/+$/, "")}/v1/tasks`, {
    signal: rpcTimeoutSignal(2000),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  const data = await res.json();
  return Array.isArray(data && data.tasks) ? data.tasks : [];
}


function headerValue(headers, name) {
  if (!Array.isArray(headers)) return "";
  const needle = name.toLowerCase();
  const h = headers.find((x) => String(x?.name || "").toLowerCase() === needle);
  return String(h?.value || "");
}

function getCookieHeaderForUrl(url) {
  return new Promise((resolve) => {
    try {
      chrome.cookies.getAll({ url }, (cookies) => {
        if (!Array.isArray(cookies) || cookies.length === 0) {
          resolve("");
          return;
        }
        const pairs = cookies
          .filter((c) => c && typeof c.name === "string" && typeof c.value === "string")
          .map((c) => `${c.name}=${c.value}`);
        resolve(pairs.join("; "));
      });
    } catch {
      resolve("");
    }
  });
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

/** 从 URL 或下载文件名提取扩展名（小写，无点） */
function getExtensionForDownload(url, filename) {
  const source = String(filename || "") || getFilenameFromUrl(url);
  const m = source.match(/\.([a-z0-9]{1,10})(?:$|[?#])/i);
  return m ? m[1].toLowerCase() : "";
}

/** 拦截过滤器：扩展名清单为空 = 全部接管（0.1.0 行为） */
function shouldIntercept(item, settings) {
  const list = Array.isArray(settings.interceptExtensions) ? settings.interceptExtensions : [];
  if (list.length === 0) return true;
  const ext = getExtensionForDownload(item.url, item.filename);
  return ext !== "" && list.map((x) => String(x).toLowerCase().replace(/^\./, "")).includes(ext);
}

const MENU_LINK = "falcon-link";
const MENU_PAGE = "falcon-page-links";

chrome.runtime.onInstalled.addListener(async () => {
  const current = await getSettings();
  await storageSet(current);

  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: MENU_LINK,
      title: chrome.i18n.getMessage("ctxDownloadLink") || "Download with Falcon",
      contexts: ["link", "video", "audio"],
    });
    chrome.contextMenus.create({
      id: MENU_PAGE,
      title: chrome.i18n.getMessage("ctxCollectPageLinks") || "Collect page links with Falcon",
      contexts: ["page"],
    });
  });
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

  if (!shouldIntercept(item, settings)) return;

  if (typeof item.tabId === "number" && item.tabId >= 0) {
    const skip = await consumeSkipOnceForTab(item.tabId);
    if (skip) return;
  }

  let ok = false;
  try {
    ok = await sendDownload(settings, {
      url: item.url,
      referrer: item.referrer || "",
      filename: item.filename || "",
    });
  } catch {
    // daemon 不可达 / RPC 错误——按发送失败收口，浏览器下载照常
  }

  if (!ok) {
    if (settings.launchFalconIfUnavailable && settings.sendTarget !== "daemon") {
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

chrome.webRequest.onBeforeSendHeaders.addListener(
  async (details) => {
    try {
      if (!details || typeof details.tabId !== "number" || details.tabId < 0) return;
      if (!details.url || details.url.startsWith("blob:")) return;

      const settings = await getSettings();
      if (!settings.sniffMedia) return;

      const host = getHostnameSafe(details.url);
      if (host && isHostDisabled(host, settings.disabledHosts)) return;

      const classification = classifyMediaUrl(details.url, "");
      if (!classification) return;

      if (!settings.sniffIncludeSegments && isLikelySegmentUrl(details.url)) {
        return;
      }

      const referrer = headerValue(details.requestHeaders, "referer");
      const userAgent = headerValue(details.requestHeaders, "user-agent");
      await upsertMediaItemForTab(
        details.tabId,
        {
          url: details.url,
          kind: classification.kind,
          label: classification.label,
          requestReferrer: referrer,
          requestUserAgent: userAgent,
          seenAt: Date.now(),
        },
        Math.max(10, Math.min(400, settings.sniffMaxItemsPerTab || DEFAULT_SETTINGS.sniffMaxItemsPerTab)),
      );
    } catch {
      // Ignore sniffer errors.
    }
  },
  { urls: ["<all_urls>"] },
  ["requestHeaders", "extraHeaders"],
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
      const cookies = settings.includeCookiesOnSend ? await getCookieHeaderForUrl(msg.url) : "";
      const cookiesSafe = cookies.length > 8192 ? cookies.slice(0, 8192) : cookies;
      let ok = false;
      let error = "";
      try {
        ok = await sendDownload(settings, {
          url: msg.url,
          referrer: msg.referrer || "",
          filename: msg.filename || "",
          user_agent: msg.userAgent || "",
          cookies: cookiesSafe,
        });
      } catch (e) {
        error = String((e && e.message) || e);
      }
      if (!ok && settings.launchFalconIfUnavailable && settings.sendTarget !== "daemon") {
        await tryLaunchFalcon(msg.url);
      }
      sendResponse({ ok, error });
      return;
    }
    if (msg?.type === "sendUrlsToFalcon" && Array.isArray(msg.urls)) {
      const settings = await getSettings();
      let sent = 0;
      let firstError = "";
      for (const url of msg.urls) {
        if (typeof url !== "string" || !url) continue;
        try {
          if (await sendDownload(settings, { url, referrer: msg.referrer || "" })) {
            sent++;
          }
        } catch (e) {
          if (!firstError) firstError = String((e && e.message) || e);
        }
      }
      sendResponse({ ok: sent > 0, sent, error: firstError });
      return;
    }
    if (msg?.type === "getTasks") {
      const settings = await getSettings();
      try {
        const tasks = await getTaskList(settings);
        sendResponse({ ok: true, tasks, target: settings.sendTarget });
      } catch (e) {
        sendResponse({ ok: false, error: String((e && e.message) || e) });
      }
      return;
    }
    if (msg?.type === "taskAction" && typeof msg.id === "string") {
      const settings = await getSettings();
      if (settings.sendTarget !== "daemon") {
        sendResponse({ ok: false, error: "Desktop mode has no remote control; use the Falcon app." });
        return;
      }
      const method =
        msg.action === "pause"
          ? "aria2.forcePause"
          : msg.action === "resume"
            ? "aria2.unpause"
            : msg.action === "remove"
              ? "aria2.remove"
              : "";
      if (!method) {
        sendResponse({ ok: false, error: "Unknown action" });
        return;
      }
      try {
        await daemonRpc(settings.daemonUrl, settings.daemonSecret, method, [msg.id]);
        sendResponse({ ok: true });
      } catch (e) {
        sendResponse({ ok: false, error: String((e && e.message) || e) });
      }
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

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (!info || typeof info.menuItemId !== "string") return;
  if (info.menuItemId !== MENU_LINK && info.menuItemId !== MENU_PAGE) return;

  const settings = await getSettings();
  if (!settings.enabled) return;

  if (info.menuItemId === MENU_LINK) {
    const url = info.linkUrl || info.srcUrl || "";
    if (!url) return;
    try {
      await sendDownload(settings, {
        url,
        referrer: info.pageUrl || (tab && tab.url) || "",
      });
      await notify("Falcon", "Sent download to Falcon.");
    } catch (e) {
      await notify("Falcon", `Send failed: ${String((e && e.message) || e)}`);
    }
    return;
  }

  // MENU_PAGE：打开批量链接选择页
  const tabId = tab && typeof tab.id === "number" ? tab.id : "";
  await tabsCreate({ url: chrome.runtime.getURL(`batch/batch.html?tabId=${tabId}`) });
});
