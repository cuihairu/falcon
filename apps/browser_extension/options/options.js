const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  sniffMedia: true,
  sniffIncludeSegments: false,
  sniffMaxItemsPerTab: 80,
  includeCookiesOnSend: false,
  disabledHosts: [],
  sendTarget: "desktop",
  daemonUrl: "http://127.0.0.1:6800",
  daemonSecret: "",
  // 与 service_worker.js 的 DEFAULT_SETTINGS.interceptExtensions 保持一致（单一事实源在
  // service_worker.js；本页需要一份同值默认才能在文本框里展示给用户编辑）。
  interceptExtensions: [
    "zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst",
    "exe", "msi", "dmg", "pkg", "deb", "rpm", "apk", "appimage", "iso",
    "mp4", "mkv", "mp3", "flac",
    "pdf", "epub",
  ],
  // 下载大小下限（字节）：小于该值的下载不接管；0 = 不限制
  interceptMinBytes: 1048576,
  // 用户是否编辑过接管清单（升级迁移标记）
  interceptTouched: false,
});

function i18n(key) {
  return api.i18n.getMessage(key) || key;
}

function storageGet(keys) {
  return new Promise((resolve) => api.storage.sync.get(keys, resolve));
}

function storageSet(values) {
  return new Promise((resolve) => api.storage.sync.set(values, resolve));
}

function toLines(text) {
  return text
    .split(/\r?\n/)
    .map((s) => s.trim().replace(/^\./, "").toLowerCase())
    .filter((s) => s.length > 0);
}

async function loadSettings() {
  const stored = await storageGet(Object.keys(DEFAULT_SETTINGS));
  const merged = {
    ...DEFAULT_SETTINGS,
    ...stored,
    disabledHosts: Array.isArray(stored.disabledHosts) ? stored.disabledHosts : DEFAULT_SETTINGS.disabledHosts,
  };
  // 与 service_worker.js 的迁移同语义：旧版默认空清单（= 全部接管）按新安全白名单展示；
  // 用户编辑过（interceptTouched）则尊重其清单。
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

/** 输入框以 MB 展示，返回 MB 数值（非法/空回落默认 1MB） */
function parseMinMb(text) {
  const t = String(text).trim();
  const fallback = DEFAULT_SETTINGS.interceptMinBytes / (1024 * 1024);
  if (t === "") return fallback;
  const n = Number(t);
  if (!Number.isFinite(n) || n < 0) return fallback;
  return n;
}

async function main() {
  document.title = i18n("optionsTitle");
  document.getElementById("title").textContent = i18n("optionsTitle");
  document.getElementById("enabledLabel").textContent = i18n("enabledLabel");
  document.getElementById("sniffMediaLabel").textContent = i18n("sniffMediaLabel");
  document.getElementById("sniffIncludeSegmentsLabel").textContent = i18n("sniffIncludeSegmentsLabel");
  document.getElementById("disabledSites").textContent = i18n("disabledSites");
  document.getElementById("apiEndpoint").textContent = i18n("apiEndpoint");
  document.getElementById("launchIfUnavailable").textContent = i18n("launchIfUnavailable");
  document.getElementById("includeCookiesOnSendLabel").textContent = i18n("includeCookiesOnSend");
  document.getElementById("sendTargetLabel").textContent = i18n("sendTargetLabel");
  document.getElementById("sendTargetDesktop").textContent = i18n("sendTargetDesktop");
  document.getElementById("sendTargetDaemon").textContent = i18n("sendTargetDaemon");
  document.getElementById("daemonUrlLabel").textContent = i18n("daemonUrlLabel");
  document.getElementById("daemonSecretLabel").textContent = i18n("daemonSecretLabel");
  document.getElementById("interceptFilterLabel").textContent = i18n("interceptFilterLabel");
  document.getElementById("interceptFilterHint").textContent = i18n("interceptFilterHint");
  document.getElementById("interceptMinBytesLabel").textContent = i18n("interceptMinBytesLabel");
  document.getElementById("interceptMinBytesHint").textContent = i18n("interceptMinBytesHint");
  document.getElementById("save").textContent = i18n("save");

  const settings = await loadSettings();

  const enabledEl = document.getElementById("enabled");
  const sniffMediaEl = document.getElementById("sniffMedia");
  const sniffIncludeSegmentsEl = document.getElementById("sniffIncludeSegments");
  const apiEl = document.getElementById("apiBaseUrl");
  const launchEl = document.getElementById("launchFalconIfUnavailable");
  const cookiesEl = document.getElementById("includeCookiesOnSend");
  const disabledEl = document.getElementById("disabledHosts");
  const daemonUrlEl = document.getElementById("daemonUrl");
  const daemonSecretEl = document.getElementById("daemonSecret");
  const interceptEl = document.getElementById("interceptExtensions");
  const interceptMinEl = document.getElementById("interceptMinBytes");
  const statusEl = document.getElementById("status");

  enabledEl.checked = !!settings.enabled;
  sniffMediaEl.checked = settings.sniffMedia !== false;
  sniffIncludeSegmentsEl.checked = !!settings.sniffIncludeSegments;
  apiEl.value = settings.apiBaseUrl || DEFAULT_SETTINGS.apiBaseUrl;
  launchEl.checked = settings.launchFalconIfUnavailable !== false;
  cookiesEl.checked = !!settings.includeCookiesOnSend;
  disabledEl.value = (settings.disabledHosts || []).join("\n");
  daemonUrlEl.value = settings.daemonUrl || DEFAULT_SETTINGS.daemonUrl;
  daemonSecretEl.value = settings.daemonSecret || "";
  interceptEl.value = (settings.interceptExtensions || []).join("\n");
  // 以 MB 显示（保留两位小数），底层存字节数
  interceptMinEl.value = (Number(settings.interceptMinBytes) / (1024 * 1024)).toFixed(2);

  const target = settings.sendTarget === "daemon" ? "daemon" : "desktop";
  document.querySelector(`input[name="sendTarget"][value="${target}"]`).checked = true;

  document.getElementById("save").addEventListener("click", async () => {
    const chosen = document.querySelector('input[name="sendTarget"]:checked');
    const next = {
      enabled: enabledEl.checked,
      sniffMedia: sniffMediaEl.checked,
      sniffIncludeSegments: sniffIncludeSegmentsEl.checked,
      sniffMaxItemsPerTab: settings.sniffMaxItemsPerTab || DEFAULT_SETTINGS.sniffMaxItemsPerTab,
      apiBaseUrl: apiEl.value.trim() || DEFAULT_SETTINGS.apiBaseUrl,
      launchFalconIfUnavailable: launchEl.checked,
      includeCookiesOnSend: cookiesEl.checked,
      disabledHosts: toLines(disabledEl.value),
      sendTarget: chosen ? chosen.value : DEFAULT_SETTINGS.sendTarget,
      daemonUrl: daemonUrlEl.value.trim() || DEFAULT_SETTINGS.daemonUrl,
      daemonSecret: daemonSecretEl.value,
      interceptExtensions: toLines(interceptEl.value),
      interceptMinBytes: Math.round(parseMinMb(interceptMinEl.value) * 1024 * 1024),
      interceptTouched: true,
    };
    await storageSet(next);
    statusEl.textContent = i18n("saved");
    setTimeout(() => (statusEl.textContent = ""), 1200);
  });
}

main();
