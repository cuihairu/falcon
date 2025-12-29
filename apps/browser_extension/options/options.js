const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  apiBaseUrl: "http://127.0.0.1:51337",
  launchFalconIfUnavailable: true,
  sniffMedia: true,
  sniffIncludeSegments: false,
  sniffMaxItemsPerTab: 80,
  includeCookiesOnSend: false,
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
  document.getElementById("sniffMediaLabel").textContent = i18n("sniffMediaLabel");
  document.getElementById("sniffIncludeSegmentsLabel").textContent = i18n("sniffIncludeSegmentsLabel");
  document.getElementById("disabledSites").textContent = i18n("disabledSites");
  document.getElementById("apiEndpoint").textContent = i18n("apiEndpoint");
  document.getElementById("launchIfUnavailable").textContent = i18n("launchIfUnavailable");
  document.getElementById("includeCookiesOnSendLabel").textContent = i18n("includeCookiesOnSend");
  document.getElementById("save").textContent = i18n("save");

  const settings = await loadSettings();

  const enabledEl = document.getElementById("enabled");
  const sniffMediaEl = document.getElementById("sniffMedia");
  const sniffIncludeSegmentsEl = document.getElementById("sniffIncludeSegments");
  const apiEl = document.getElementById("apiBaseUrl");
  const launchEl = document.getElementById("launchFalconIfUnavailable");
  const cookiesEl = document.getElementById("includeCookiesOnSend");
  const disabledEl = document.getElementById("disabledHosts");
  const statusEl = document.getElementById("status");

  enabledEl.checked = !!settings.enabled;
  sniffMediaEl.checked = settings.sniffMedia !== false;
  sniffIncludeSegmentsEl.checked = !!settings.sniffIncludeSegments;
  apiEl.value = settings.apiBaseUrl || DEFAULT_SETTINGS.apiBaseUrl;
  launchEl.checked = settings.launchFalconIfUnavailable !== false;
  cookiesEl.checked = !!settings.includeCookiesOnSend;
  disabledEl.value = (settings.disabledHosts || []).join("\n");

  document.getElementById("save").addEventListener("click", async () => {
    const next = {
      enabled: enabledEl.checked,
      sniffMedia: sniffMediaEl.checked,
      sniffIncludeSegments: sniffIncludeSegmentsEl.checked,
      sniffMaxItemsPerTab: settings.sniffMaxItemsPerTab || DEFAULT_SETTINGS.sniffMaxItemsPerTab,
      apiBaseUrl: apiEl.value.trim() || DEFAULT_SETTINGS.apiBaseUrl,
      launchFalconIfUnavailable: launchEl.checked,
      includeCookiesOnSend: cookiesEl.checked,
      disabledHosts: toLines(disabledEl.value),
    };
    await storageSet(next);
    statusEl.textContent = i18n("saved");
    setTimeout(() => (statusEl.textContent = ""), 1200);
  });
}

main();
