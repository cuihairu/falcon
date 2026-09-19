const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  sniffMedia: true,
  sniffIncludeSegments: false,
  sniffMaxItemsPerTab: 80,
  disabledHosts: [],
  sendTarget: "desktop",
  daemonUrl: "http://127.0.0.1:6800",
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
  document.getElementById("tasksTitle").textContent = i18n("tasksTitle");
  document.getElementById("tasksRefresh").textContent = i18n("refresh");
  document.getElementById("collectLinks").textContent = i18n("collectLinks");

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

  //--------------------------------------------------------------------------
  // 任务面板（desktop → /v1/tasks 快照；daemon → aria2 tell* 聚合）
  //--------------------------------------------------------------------------

  const taskListEl = document.getElementById("taskList");
  const tasksStatusEl = document.getElementById("tasksStatus");

  const STATUS_KEY = {
    active: "stActive",
    waiting: "stWaiting",
    paused: "stPaused",
    complete: "stComplete",
    error: "stError",
    removed: "stRemoved",
  };

  function formatBytes(n) {
    const v = Number(n) || 0;
    if (v >= 1024 * 1024 * 1024) return `${(v / 1024 / 1024 / 1024).toFixed(1)} GB`;
    if (v >= 1024 * 1024) return `${(v / 1024 / 1024).toFixed(1)} MB`;
    if (v >= 1024) return `${(v / 1024).toFixed(0)} KB`;
    return `${v} B`;
  }

  function taskName(task) {
    const fromPath = String(task.path || "").split(/[\\/]/).filter(Boolean).pop() || "";
    const fromUrl = getHostname(task.url || "");
    return fromPath || fromUrl || task.id || "-";
  }

  async function refreshTasks() {
    taskListEl.replaceChildren();
    tasksStatusEl.textContent = i18n("tasksLoading");

    const res = await chrome.runtime.sendMessage({ type: "getTasks" });
    if (!res || !res.ok) {
      tasksStatusEl.textContent = `${i18n("failed")}${res && res.error ? `: ${res.error}` : ""}`;
      return;
    }

    const tasks = Array.isArray(res.tasks) ? res.tasks : [];
    if (tasks.length === 0) {
      tasksStatusEl.textContent = i18n("tasksEmpty");
      return;
    }
    tasksStatusEl.textContent = "";

    const isDaemon = res.target === "daemon";

    for (const task of tasks.slice(0, 12)) {
      const row = document.createElement("div");
      row.className = "taskItem";

      const head = document.createElement("div");
      head.className = "taskHead";

      const name = document.createElement("span");
      name.className = "taskName";
      name.title = task.url || task.path || "";
      name.textContent = taskName(task);

      const status = document.createElement("span");
      status.className = `taskStatus st_${task.status || "unknown"}`;
      status.textContent = i18n(STATUS_KEY[task.status] || "stUnknown");

      head.append(name, status);

      const bar = document.createElement("div");
      bar.className = "taskBar";
      const fill = document.createElement("div");
      fill.className = "taskBarFill";
      const pct = Math.max(0, Math.min(100, (Number(task.progress) || 0) * 100));
      fill.style.width = `${pct.toFixed(1)}%`;
      bar.append(fill);

      const meta = document.createElement("div");
      meta.className = "taskMeta";
      const speed = Number(task.speed) || 0;
      meta.textContent = [
        `${pct.toFixed(0)}%`,
        `${formatBytes(task.downloadedBytes)} / ${formatBytes(task.totalBytes)}`,
        speed > 0 ? `${formatBytes(speed)}/s` : "",
      ]
        .filter(Boolean)
        .join(" · ");

      row.append(head, bar, meta);

      if (isDaemon && (task.status === "active" || task.status === "waiting" || task.status === "paused")) {
        const actions = document.createElement("div");
        actions.className = "mediaActions";

        const canPause = task.status === "active" || task.status === "waiting";
        const canResume = task.status === "paused";

        if (canPause) {
          const btn = document.createElement("button");
          btn.textContent = i18n("pauseTask");
          btn.addEventListener("click", async () => {
            btn.disabled = true;
            await chrome.runtime.sendMessage({ type: "taskAction", action: "pause", id: task.id });
            await refreshTasks();
          });
          actions.append(btn);
        }
        if (canResume) {
          const btn = document.createElement("button");
          btn.textContent = i18n("resumeTask");
          btn.addEventListener("click", async () => {
            btn.disabled = true;
            await chrome.runtime.sendMessage({ type: "taskAction", action: "resume", id: task.id });
            await refreshTasks();
          });
          actions.append(btn);
        }
        row.append(actions);
      }

      taskListEl.appendChild(row);
    }
  }

  document.getElementById("tasksRefresh").addEventListener("click", refreshTasks);

  document.getElementById("collectLinks").addEventListener("click", async () => {
    if (typeof tab?.id !== "number") return;
    await chrome.tabs.create({
      url: chrome.runtime.getURL(`batch/batch.html?tabId=${tab.id}`),
    });
    window.close();
  });

  await refreshTasks();
  await refreshMediaList();
}

main();
