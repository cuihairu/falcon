/* ==========================================================================
   Falcon UI · ui-next 原型交互
   1) 主题色 / 亮暗切换（写 data-theme / data-mode 到 <html>）
   2) 侧栏导航 + data-go 内部跳转
   3) 开关、复选、匹配码输入等轻交互
   ========================================================================== */
(function () {
  "use strict";

  var root = document.documentElement;

  /* ---------- 主题切换（审查工具条 + 设置页外观区，双向联动）---------- */
  var THEME_NAMES = { warm: "暖橙（默认）", cold: "冷蓝", graphite: "石墨绿" };

  function setTheme(theme) {
    root.setAttribute("data-theme", theme);
    // 同步所有色块：审查工具条 .swatch 与设置页 .q-swatch
    document.querySelectorAll(".swatch, .q-swatch").forEach(function (s) {
      s.classList.toggle("active", s.getAttribute("data-set") === theme);
    });
    var name = THEME_NAMES[theme];
    var note = document.querySelector(".scale-note");
    if (note) note.textContent = "当前：" + name + " · 1120 × 720 预览";
    var label = document.getElementById("themeName");
    if (label) label.textContent = name;
  }

  // 主题模式：light / dark / system（跟随系统深色偏好）
  var modePref = "light";
  var mediaDark = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  function applyMode() {
    var effective = modePref === "system"
      ? (mediaDark && mediaDark.matches ? "dark" : "light")
      : modePref;
    root.setAttribute("data-mode", effective);
  }
  function setMode(mode) {
    modePref = mode;
    applyMode();
    // 同步审查工具条与设置页的模式按钮
    document.querySelectorAll(".mode-toggle button").forEach(function (b) {
      b.classList.toggle("active", b.getAttribute("data-mode") === mode);
    });
  }
  // 系统主题变化时，若当前为「跟随系统」则实时应用
  if (mediaDark) {
    var onSysChange = function () { if (modePref === "system") applyMode(); };
    if (mediaDark.addEventListener) mediaDark.addEventListener("change", onSysChange);
    else if (mediaDark.addListener) mediaDark.addListener(onSysChange);
  }

  // 工具条色块 + 设置页色块
  document.querySelectorAll(".swatch, .q-swatch").forEach(function (s) {
    s.addEventListener("click", function () { setTheme(s.getAttribute("data-set")); });
  });
  // 工具条 + 设置页的模式按钮
  document.querySelectorAll(".mode-toggle button").forEach(function (b) {
    b.addEventListener("click", function () { setMode(b.getAttribute("data-mode")); });
  });

  /* ---------- 全局搜索（顶栏「搜文件、贴链接」）---------- */
  var gs = document.getElementById("globalSearch");
  var searchInput = document.getElementById("searchInput");
  var searchPanel = document.getElementById("searchPanel");
  var spEmpty = document.getElementById("spEmpty");
  var spTabs = document.getElementById("spTabs");
  var SAMPLE_RESULTS = [
    { tag: "链接", name: "Ubuntu 24.04 LTS 官方镜像", meta: "2.8 GB · 1337x" },
    { tag: "云盘", name: "项目资料归档.zip", meta: "156 MB · 云盘空间" },
    { tag: "下载", name: "Blender 4.2 安装包", meta: "312 MB · 已完成" }
  ];

  function openSearch() { if (gs) gs.classList.add("open"); }
  function closeSearch() { if (gs) gs.classList.remove("open"); }
  function renderResults(q) {
    if (!spEmpty) return;
    if (!q) { spEmpty.textContent = "输入关键词开始搜索"; spEmpty.className = "sp-empty"; return; }
    spEmpty.className = "sp-results";
    spEmpty.innerHTML = SAMPLE_RESULTS.map(function (r) {
      return '<div class="sp-result"><span class="r-tag">' + r.tag + '</span><span>' + q + ' · ' + r.name + '</span><span class="r-meta">' + r.meta + '</span></div>';
    }).join("");
  }
  if (searchInput) {
    searchInput.addEventListener("focus", openSearch);
    searchInput.addEventListener("input", function () { renderResults(searchInput.value.trim()); });
    searchInput.addEventListener("keydown", function (e) {
      if (e.key === "Enter" && searchInput.value.trim()) { closeSearch(); showScreen("discovery"); }
      if (e.key === "Escape") { searchInput.blur(); closeSearch(); }
    });
  }
  if (gs) {
    gs.addEventListener("click", function (e) {
      if (e.target.closest(".search-panel")) return;
      openSearch();
      if (searchInput) searchInput.focus();
    });
  }
  // 点击面板外收起
  var topbarNew = document.querySelector(".topbar-new");
  document.addEventListener("click", function (e) {
    if (gs && !gs.contains(e.target)) closeSearch();
    if (topbarNew && !topbarNew.contains(e.target)) topbarNew.classList.remove("open");
  });

  /* ---------- 顶栏「新建」下拉菜单 ---------- */

  var newBtn = document.getElementById("newBtn");
  var newMenu = document.getElementById("newMenu");
  if (newBtn && topbarNew) {
    newBtn.addEventListener("click", function (e) {
      e.stopPropagation();
      topbarNew.classList.toggle("open");
      if (gs) gs.classList.remove("open");
    });
  }
  if (newMenu) {
    newMenu.addEventListener("click", function (e) {
      var item = e.target.closest("button[data-go]");
      if (item) topbarNew.classList.remove("open");
    });
  }
  // 分类标签切换
  if (spTabs) {
    spTabs.addEventListener("click", function (e) {
      var btn = e.target.closest("button");
      if (!btn) return;
      spTabs.querySelectorAll("button").forEach(function (b) { b.classList.remove("active"); });
      btn.classList.add("active");
    });
  }
  // 「打开浏览器全网搜」→ 资源发现页
  var spOpenWeb = document.getElementById("spOpenWeb");
  if (spOpenWeb) spOpenWeb.addEventListener("click", function () { closeSearch(); showScreen("discovery"); });
  var gsGlobe = document.getElementById("gsGlobe");
  if (gsGlobe) gsGlobe.addEventListener("click", function () { closeSearch(); showScreen("discovery"); });

  /* ---------- 界面导航 ---------- */
  var screens = Array.prototype.slice.call(document.querySelectorAll(".screen"));
  var navItems = Array.prototype.slice.call(document.querySelectorAll(".nav-item"));

  // 卡片视图与列表视图同属「下载中」，共用同一侧栏锚点
  var NAV_ALIAS = { "download-card": "download-table" };
  function navKeyFor(name) { return NAV_ALIAS[name] || name; }

  function showScreen(name, fromNav) {
    var target = screens.filter(function (s) { return s.getAttribute("data-screen") === name; })[0];
    if (!target) return;

    screens.forEach(function (s) { s.classList.remove("active"); });
    target.classList.add("active");

    // 侧栏高亮：视图别名归一化后匹配导航项
    var navKey = navKeyFor(name);
    var nav = navItems.filter(function (n) { return n.getAttribute("data-screen") === navKey; })[0];
    navItems.forEach(function (n) { n.classList.remove("active"); });
    if (nav && fromNav !== false) nav.classList.add("active");

    var content = document.getElementById("content");
    if (content) content.scrollTop = 0;

  }

  navItems.forEach(function (item) {
    item.addEventListener("click", function () {
      showScreen(item.getAttribute("data-screen"), true);
    });
  });

  // 内容区内的 data-go 按钮（跨界面跳转，不强制高亮侧栏）
  document.querySelectorAll("[data-go]").forEach(function (el) {
    el.addEventListener("click", function () {
      var name = el.getAttribute("data-go");
      showScreen(name);
      // 若带 data-anchor，跳转后滚动到目标分区
      var anchor = el.getAttribute("data-anchor");
      if (anchor) {
        var target = document.getElementById(anchor);
        var content = document.getElementById("content");
        if (target && content) {
          content.scrollTop = Math.max(0, target.offsetTop - content.offsetTop - 8);
        }
      }
    });
  });

  /* ---------- 轻交互 ---------- */
  // 开关
  document.querySelectorAll("[data-sw]").forEach(function (sw) {
    sw.addEventListener("click", function () { sw.classList.toggle("on"); });
  });
  // 复选
  document.querySelectorAll("[data-ck]").forEach(function (ck) {
    ck.addEventListener("click", function () {
      ck.classList.toggle("on");
      ck.textContent = ck.classList.contains("on") ? "✓" : "";
    });
  });
  // 已启用资源站：默认折叠，点击图标入口展开/收起
  var engineSection = document.getElementById("engineSection");
  var engineSummary = document.getElementById("engineSummary");
  if (engineSummary && engineSection) {
    engineSummary.addEventListener("click", function () {
      var expanded = engineSection.classList.toggle("expanded");
      engineSummary.setAttribute("aria-expanded", expanded ? "true" : "false");
    });
  }
  // 分段按钮组内单选（外观主题色预览等）
  document.querySelectorAll(".segmented").forEach(function (seg) {
    seg.addEventListener("click", function (e) {
      var btn = e.target.closest("button");
      if (!btn || btn.hasAttribute("data-go")) return;
      seg.querySelectorAll("button").forEach(function (b) { b.classList.remove("active"); });
      btn.classList.add("active");
    });
  });
  // 匹配码：自动跳到下一格 + 只允许数字
  var codeInputs = Array.prototype.slice.call(document.querySelectorAll(".code-inputs .ci"));
  codeInputs.forEach(function (ci, idx) {
    ci.addEventListener("input", function () {
      ci.value = ci.value.replace(/\D/g, "").slice(0, 1);
      if (ci.value && idx < codeInputs.length - 1) codeInputs[idx + 1].focus();
    });
    ci.addEventListener("keydown", function (e) {
      if (e.key === "Backspace" && !ci.value && idx > 0) codeInputs[idx - 1].focus();
    });
  });

  /* ---------- 配对：扫码 / 手动输入（同一串码的两种形态）---------- */
  var pairTabs = document.getElementById("pairTabs");
  if (pairTabs) {
    pairTabs.addEventListener("click", function (e) {
      var btn = e.target.closest("button[data-tab]");
      if (!btn) return;
      var tab = btn.getAttribute("data-tab");
      pairTabs.querySelectorAll("button").forEach(function (b) { b.classList.toggle("active", b === btn); });
      document.querySelectorAll(".pair-pane").forEach(function (p) {
        p.classList.toggle("active", p.getAttribute("data-pane") === tab);
      });
    });
  }

  /* ---------- 展示传送码：二维码 + 6 位码 + 倒计时 ---------- */
  var CODE_ALPHABET = "23456789ABCDEFGHJKMPQRSTUVWXYZ"; // 去掉 0/O/1/I/L 等易混字符
  var qrGrid = document.getElementById("qrGrid");
  var bigCode = document.getElementById("bigCode");
  var countdown = document.getElementById("qrCountdown");
  var codeText = "482 913";
  var secondsLeft = 120;
  var ticker = null;

  function newCode() {
    var pick = function () { return CODE_ALPHABET[Math.floor(Math.random() * CODE_ALPHABET.length)]; };
    var raw = pick() + pick() + pick() + pick() + pick() + pick();
    codeText = raw.slice(0, 3) + " " + raw.slice(3);
    if (bigCode) bigCode.textContent = codeText;
    secondsLeft = 120;
    drawQR();
  }
  function drawQR() {
    if (!qrGrid) return;
    var seed = codeText.replace(/\s/g, "");
    var html = "";
    for (var y = 0; y < 11; y++) {
      for (var x = 0; x < 11; x++) {
        var hash = (seed.charCodeAt((x + y * 3) % seed.length) * (x + 3) * (y + 5)) % 7;
        if (hash < 3) html += '<i style="left:' + (x * 9.09) + '%;top:' + (y * 9.09) + '%;width:8%;height:8%"></i>';
      }
    }
    qrGrid.innerHTML = html;
  }
  function startTicker() {
    if (ticker) clearInterval(ticker);
    if (!countdown) return;
    ticker = setInterval(function () {
      secondsLeft = secondsLeft > 0 ? secondsLeft - 1 : 0;
      countdown.textContent = secondsLeft > 0 ? secondsLeft + " 秒后失效" : "已失效 · 请刷新";
      if (secondsLeft === 0 && ticker) { clearInterval(ticker); ticker = null; }
    }, 1000);
  }
  var refreshCode = document.getElementById("refreshCode");
  if (refreshCode) refreshCode.addEventListener("click", newCode);
  var copyCode = document.getElementById("copyCode");
  if (copyCode) {
    copyCode.addEventListener("click", function () {
      var text = codeText.replace(/\s/g, "");
      if (navigator.clipboard) navigator.clipboard.writeText(text);
      copyCode.textContent = "已复制 " + text;
      setTimeout(function () { copyCode.textContent = "复制码"; }, 1600);
    });
  }
  drawQR();
  startTicker();

  // 悬浮速度窗：迷你胶囊 ↔ 详细卡片，点击时标注当前态
  document.querySelectorAll("[data-fs-toggle], [data-fs-collapse]").forEach(function (el) {
    el.addEventListener("click", function () {
      var demo = el.closest(".fs-demo");
      if (!demo) return;
      demo.querySelectorAll(".fs-state").forEach(function (s) { s.classList.remove("fs-active"); });
      var target = el.classList.contains("fs-collapse") ? el.closest(".fs-state") : el.closest(".fs-state").nextElementSibling;
      if (target) target.classList.add("fs-active");
    });
  });

  /* ---------- 键盘快捷键：← → 逐屏审查 ---------- */
  document.addEventListener("keydown", function (e) {
    if (e.target.matches("input, textarea")) return;
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    var current = screens.findIndex(function (s) { return s.classList.contains("active"); });
    var next = e.key === "ArrowRight" ? current + 1 : current - 1;
    if (next < 0 || next >= screens.length) return;
    var name = screens[next].getAttribute("data-screen");
    showScreen(name);
    // 同步侧栏高亮（若存在）
    navItems.forEach(function (n) {
      n.classList.toggle("active", n.getAttribute("data-screen") === name);
    });
  });

  // 初始化
  var params = new URLSearchParams(location.search);
  if (params.get("clean") === "1") document.body.classList.add("export-clean");
  setTheme(params.get("theme") || "warm");
  setMode(params.get("mode") || "light");
  var initialScreen = params.get("screen");
  if (initialScreen) showScreen(initialScreen);
})();
