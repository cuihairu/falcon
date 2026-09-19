function i18n(key) {
  return chrome.i18n.getMessage(key) || key;
}

const MAX_LINKS = 500;

let allLinks = [];
let sourceTabId = null;
let sourceTabUrl = "";

/** 从目标 tab 提取全部 http(s) 链接（去重，附链接文本） */
async function collectLinks(tabId) {
  const results = await chrome.scripting.executeScript({
    target: { tabId },
    func: (maxLinks) => {
      const seen = new Map();
      for (const a of document.querySelectorAll("a[href]")) {
        if (seen.size >= maxLinks) break;
        try {
          const u = new URL(a.href, location.href);
          if (u.protocol !== "http:" && u.protocol !== "https:") continue;
          const text = (a.textContent || "").trim().replace(/\s+/g, " ").slice(0, 100);
          const name = decodeURIComponent(u.pathname.split("/").filter(Boolean).pop() || "");
          if (!seen.has(u.href)) {
            seen.set(u.href, { url: u.href, text: text || name || u.hostname });
          }
        } catch {
          // Malformed href — skip.
        }
      }
      return [...seen.values()];
    },
    args: [MAX_LINKS],
  });
  const first = Array.isArray(results) ? results[0] : null;
  return first && Array.isArray(first.result) ? first.result : [];
}

function applyFilter() {
  const needle = document.getElementById("filter").value.trim().toLowerCase();
  const listEl = document.getElementById("linkList");
  listEl.replaceChildren();

  let visible = 0;
  let checked = 0;
  for (const link of allLinks) {
    const hay = `${link.text}\n${link.url}`.toLowerCase();
    if (needle && !hay.includes(needle)) continue;
    visible++;

    const label = document.createElement("label");
    label.className = "linkItem";

    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = link.checked;
    box.dataset.url = link.url;
    box.addEventListener("change", () => {
      link.checked = box.checked;
      updateCount();
    });

    const main = document.createElement("span");
    main.className = "linkMain";

    const text = document.createElement("span");
    text.className = "linkText";
    text.textContent = link.text;

    const url = document.createElement("span");
    url.className = "linkUrl";
    url.textContent = link.url;

    main.append(text, url);
    label.append(box, main);
    listEl.appendChild(label);
    if (link.checked) checked++;
  }

  document.getElementById("countInfo").textContent = `${checked} / ${visible}`;
}

function updateCount() {
  applyFilter();
}

function setAll(checked) {
  const needle = document.getElementById("filter").value.trim().toLowerCase();
  for (const link of allLinks) {
    if (!needle) {
      link.checked = checked;
      continue;
    }
    const hay = `${link.text}\n${link.url}`.toLowerCase();
    if (hay.includes(needle)) link.checked = checked;
  }
  applyFilter();
}

async function main() {
  document.getElementById("title").textContent = i18n("batchTitle");
  document.getElementById("filter").placeholder = i18n("batchFilterPlaceholder");
  document.getElementById("selectAll").textContent = i18n("batchSelectAll");
  document.getElementById("selectNone").textContent = i18n("batchClear");
  document.getElementById("send").textContent = i18n("batchSend");

  const params = new URLSearchParams(location.search);
  const raw = params.get("tabId");
  sourceTabId = raw !== null && raw !== "" ? Number(raw) : null;

  const statusEl = document.getElementById("statusText");
  const listEl = document.getElementById("linkList");
  const sendStatusEl = document.getElementById("sendStatus");
  const sendBtn = document.getElementById("send");

  if (sourceTabId === null || !Number.isFinite(sourceTabId)) {
    statusEl.textContent = i18n("batchEmpty");
    sendBtn.disabled = true;
    document.getElementById("sourceTab").textContent = "-";
    return;
  }

  try {
    const tab = await chrome.tabs.get(sourceTabId);
    sourceTabUrl = (tab && tab.url) || "";
  } catch {
    // Tab may be gone; proceed with empty referrer.
  }
  document.getElementById("sourceTab").textContent = sourceTabUrl || "-";

  statusEl.textContent = i18n("batchLoading");
  try {
    allLinks = await collectLinks(sourceTabId);
  } catch (e) {
    allLinks = [];
    statusEl.textContent = `${i18n("failed")}: ${String((e && e.message) || e)}`;
    sendBtn.disabled = true;
    return;
  }

  statusEl.textContent = "";
  if (allLinks.length === 0) {
    statusEl.textContent = i18n("batchEmpty");
    sendBtn.disabled = true;
    return;
  }

  sendBtn.disabled = false;
  applyFilter();

  document.getElementById("filter").addEventListener("input", applyFilter);
  document.getElementById("selectAll").addEventListener("click", () => setAll(true));
  document.getElementById("selectNone").addEventListener("click", () => setAll(false));

  sendBtn.addEventListener("click", async () => {
    const urls = allLinks.filter((x) => x.checked).map((x) => x.url);
    if (urls.length === 0) {
      sendStatusEl.textContent = i18n("batchNoneSelected");
      return;
    }
    sendBtn.disabled = true;
    sendStatusEl.textContent = "";
    const res = await chrome.runtime.sendMessage({
      type: "sendUrlsToFalcon",
      urls,
      referrer: sourceTabUrl,
    });
    if (res && res.ok) {
      sendStatusEl.textContent = `${i18n("batchSent")}: ${res.sent}/${urls.length}`;
    } else {
      sendStatusEl.textContent = `${i18n("failed")}${res && res.error ? `: ${res.error}` : ""}`;
    }
    sendBtn.disabled = false;
  });
}

main();
