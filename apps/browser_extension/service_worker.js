// Chromium loads this file as a classic MV3 service worker (see manifest
// background). Firefox loads it as the second entry of background.scripts,
// where importScripts does not exist — api.js is preloaded by the manifest
// there, so the hook below is Chromium-only and must stay guarded.
if (typeof importScripts === "function") {
  importScripts("api.js");
}

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
  // 接管浏览器下载的文件类型（小写扩展名数组）。默认安全白名单：只接管
  // 典型的大文件下载类型，避免误伤普通网页资源（0.1.0 的空数组 = 全部接管）。
  // 清单里写 "*" = 全部接管；清空并保存 = 不接管任何下载。
  interceptExtensions: [
    "zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst",
    "exe", "msi", "dmg", "pkg", "deb", "rpm", "apk", "appimage", "iso",
    "mp4", "mkv", "mp3", "flac",
    "pdf", "epub",
  ],
  // 下载大小下限（字节）：小于该值的下载不接管；0 = 不限制
  interceptMinBytes: 1048576,
  // 用户是否编辑过接管清单（升级迁移标记：未编辑过的旧空清单按新默认白名单处理）
  interceptTouched: false,
});

function storageGet(keys) {
  return new Promise((resolve) => api.storage.sync.get(keys, resolve));
}

function storageSet(values) {
  return new Promise((resolve) => api.storage.sync.set(values, resolve));
}

const sessionStorage = api.storage.session || api.storage.local;

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
  return new Promise((resolve) => api.downloads.cancel(id, () => resolve()));
}

function downloadsErase(id) {
  return new Promise((resolve) => api.downloads.erase({ id }, () => resolve()));
}

function downloadsDownload(options) {
  return new Promise((resolve) => api.downloads.download(options, resolve));
}

function notificationsCreate(options) {
  return new Promise((resolve) => api.notifications.create(options, resolve));
}

function tabsCreate(createProperties) {
  return new Promise((resolve) => api.tabs.create(createProperties, resolve));
}

function tabsRemove(tabId) {
  return new Promise((resolve) => api.tabs.remove(tabId, () => resolve()));
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
  const merged = {
    ...DEFAULT_SETTINGS,
    ...stored,
    disabledHosts: Array.isArray(stored.disabledHosts) ? stored.disabledHosts : DEFAULT_SETTINGS.disabledHosts,
  };
  // 升级迁移：接管清单为空且用户从未编辑过（interceptTouched）= 旧版默认（空 = 全部接管），
  // 按新的安全白名单处理；用户编辑过则尊重其清单（含有意清空 = 不接管任何下载）。
  if (!Array.isArray(merged.interceptExtensions)) {
    merged.interceptExtensions = [...DEFAULT_SETTINGS.interceptExtensions];
  } else if (merged.interceptExtensions.length === 0 && merged.interceptTouched !== true) {
    merged.interceptExtensions = [...DEFAULT_SETTINGS.interceptExtensions];
  }
  const minBytes = Number(merged.interceptMinBytes);
  merged.interceptMinBytes =
    Number.isFinite(minBytes) && minBytes >= 0 ? minBytes : DEFAULT_SETTINGS.interceptMinBytes;
  return merged;
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
      api.cookies.getAll({ url }, (cookies) => {
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
  // Content-Type 存在且明显是文本/HTML/JSON 时以 Content-Type 为准：URL 带媒体后缀的
  // 接口路由（如 /video/play 返回 text/html、/media.json 返回 application/json）不算媒体。
  if (ct && (ct.startsWith("text/") || ct.includes("application/json"))) return null;
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

/** 拦截过滤器：清单为空 = 不接管任何下载；清单含 "*" = 全部接管（0.1.0 旧行为） */
function shouldIntercept(item, settings) {
  const list = Array.isArray(settings.interceptExtensions) ? settings.interceptExtensions : [];
  if (list.length === 0) return false;
  const normalized = list.map((x) => String(x).trim().toLowerCase().replace(/^\./, ""));
  if (normalized.includes("*")) return true;
  const ext = getExtensionForDownload(item.url, item.filename);
  return ext !== "" && normalized.includes(ext);
}

/** 大小下限：已知尺寸小于阈值的下载不接管；0 = 不限制；尺寸未知（-1/NaN）视为满足 */
function meetsSizeFloor(sizeBytes, minBytes) {
  const floor = Number(minBytes);
  if (!Number.isFinite(floor) || floor <= 0) return true;
  const size = Number(sizeBytes);
  return !Number.isFinite(size) || size < 0 || size >= floor;
}

const MENU_LINK = "falcon-link";
const MENU_PAGE = "falcon-page-links";

api.runtime.onInstalled.addListener(async () => {
  const current = await getSettings();
  await storageSet(current);

  api.contextMenus.removeAll(() => {
    api.contextMenus.create({
      id: MENU_LINK,
      title: api.i18n.getMessage("ctxDownloadLink") || "Download with Falcon",
      contexts: ["link", "video", "audio"],
    });
    api.contextMenus.create({
      id: MENU_PAGE,
      title: api.i18n.getMessage("ctxCollectPageLinks") || "Collect page links with Falcon",
      contexts: ["page"],
    });
  });
});

api.tabs.onRemoved.addListener(async (tabId) => {
  await clearMediaForTab(tabId);
});

api.downloads.onCreated.addListener(async (item) => {
  if (!item || !item.url || typeof item.id !== "number") return;
  if (item.state === "complete") return;

  const settings = await getSettings();
  if (!settings.enabled) return;

  const host = getHostname(item.url);
  if (host && isHostDisabled(host, settings.disabledHosts)) return;

  if (!shouldIntercept(item, settings)) return;

  // 大小下限防误伤：页面小图/脚本/字体之类的小资源即使扩展名命中也不接管
  if (!meetsSizeFloor(item.fileSize, settings.interceptMinBytes)) return;

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

api.webRequest.onHeadersReceived.addListener(
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
  // `extraHeaders` is Chromium-only (Firefox rejects the value and exposes
  // those headers regardless), so gate it per engine. Both listeners here
  // are observational — no `blocking` in the spec on either engine.
  typeof browser !== "undefined"
    ? ["responseHeaders"]
    : ["responseHeaders", "extraHeaders"],
);

api.webRequest.onBeforeSendHeaders.addListener(
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
  // Same engine gate as the onHeadersReceived listener above.
  typeof browser !== "undefined"
    ? ["requestHeaders"]
    : ["requestHeaders", "extraHeaders"],
);

api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
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

api.contextMenus.onClicked.addListener(async (info, tab) => {
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
  await tabsCreate({ url: api.runtime.getURL(`batch/batch.html?tabId=${tabId}`) });
});
