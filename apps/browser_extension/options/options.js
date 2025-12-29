const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
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

function toLines(text) {
  return text
    .split(/\r?\n/)
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
}

async function loadSettings() {
  const stored = await storageGet(Object.keys(DEFAULT_SETTINGS));
  return {
    ...DEFAULT_SETTINGS,
    ...stored,
    disabledHosts: Array.isArray(stored.disabledHosts) ? stored.disabledHosts : DEFAULT_SETTINGS.disabledHosts,
  };
}

async function main() {
  document.title = i18n("optionsTitle");
  document.getElementById("title").textContent = i18n("optionsTitle");
  document.getElementById("enabledLabel").textContent = i18n("enabledLabel");
  document.getElementById("disabledSites").textContent = i18n("disabledSites");
  document.getElementById("apiEndpoint").textContent = i18n("apiEndpoint");
  document.getElementById("launchIfUnavailable").textContent = i18n("launchIfUnavailable");
  document.getElementById("save").textContent = i18n("save");

  const settings = await loadSettings();

  const enabledEl = document.getElementById("enabled");
  const apiEl = document.getElementById("apiBaseUrl");
  const launchEl = document.getElementById("launchFalconIfUnavailable");
  const disabledEl = document.getElementById("disabledHosts");
  const statusEl = document.getElementById("status");

  enabledEl.checked = !!settings.enabled;
  apiEl.value = settings.apiBaseUrl || DEFAULT_SETTINGS.apiBaseUrl;
  launchEl.checked = settings.launchFalconIfUnavailable !== false;
  disabledEl.value = (settings.disabledHosts || []).join("\n");

  document.getElementById("save").addEventListener("click", async () => {
    const next = {
      enabled: enabledEl.checked,
      apiBaseUrl: apiEl.value.trim() || DEFAULT_SETTINGS.apiBaseUrl,
      launchFalconIfUnavailable: launchEl.checked,
      disabledHosts: toLines(disabledEl.value),
    };
    await storageSet(next);
    statusEl.textContent = i18n("saved");
    setTimeout(() => (statusEl.textContent = ""), 1200);
  });
}

main();
