<script setup>
/**
 * 首页界面走马灯：设计原型 + 桌面端实况截图轮播。
 *
 * 硬性要求（用户验收点）：自动播放 + 左右箭头 + 圆点指示、
 * 宽度自适应、亮暗双主题清晰（配色全部走主题 token）。
 *
 * 实现要点：
 *  - 图片经 Vite 静态导入产出带 base 的地址（部署在 /falcon/ 子路径下
 *    也能正确解析），不依赖 markdown 层的相对路径重写。
 *  - SSG 安全：定时器/事件监听全部在 onMounted 挂载、onUnmounted 拆除，
 *    setup 期间不触碰 window/document。
 *  - prefers-reduced-motion 下禁用自动播放（与全站动效纪律一致）。
 */
import { onMounted, onUnmounted, ref, computed } from "vue";
import warmConsole from "../../screenshots/warm-console-dark.png";
import coldUtility from "../../screenshots/cold-utility-dark.png";
import downloadTable from "../../screenshots/download_table_dark_1200.png";
import completedGrid from "../../screenshots/download_completed_grid_dark_1200.png";
import addDialog from "../../screenshots/add_dialog_dark_1200.png";
import cloudBrowser from "../../screenshots/cloud_dark_1200.png";
import settingsPage from "../../screenshots/settings_dark_1200.png";

// 图片经 Vite 静态导入（相对路径），构建期产出带 base 的确定地址；
// 不要改成 "/screenshots/..." 公共路径——那要求文件放进 src/public/，
// markdown 层引用与组件引用会走两套解析。
const slides = [
  {
    src: warmConsole,
    tag: "设计原型",
    caption: "主视图原型 · warm console（现行主题方向，暖黑底 + falcon 橙单强调）",
    alt: "warm console 主视图设计原型（暗色）",
  },
  {
    src: coldUtility,
    tag: "设计原型",
    caption: "主视图原型 · cold utility（备选变体，冷中性底 + 克制蓝强调）",
    alt: "cold utility 主视图设计原型（暗色）",
  },
  {
    src: downloadTable,
    tag: "实况截图",
    caption: "下载管理 · 任务表格视图（暗色主题）",
    alt: "桌面端下载页任务表格视图截图",
  },
  {
    src: completedGrid,
    tag: "实况截图",
    caption: "已完成列表 · 网格视图（暗色主题）",
    alt: "桌面端已完成任务网格视图截图",
  },
  {
    src: addDialog,
    tag: "实况截图",
    caption: "添加下载对话框（Fluent 风格）",
    alt: "桌面端添加下载对话框截图",
  },
  {
    src: cloudBrowser,
    tag: "实况截图",
    caption: "云存储浏览（S3 / OSS / COS / Kodo / 又拍云）",
    alt: "桌面端云存储浏览页截图",
  },
  {
    src: settingsPage,
    tag: "实况截图",
    caption: "设置页（下载参数 / 外观 / 连接）",
    alt: "桌面端设置页截图",
  },
];

const current = ref(0);
const count = slides.length;
const trackStyle = computed(() => ({
  transform: `translateX(-${current.value * 100}%)`,
}));

const AUTOPLAY_MS = 4500;
let timer = null;
let reducedMotion = false;
let hovering = false;

function go(i) {
  current.value = (i + count) % count;
  restart();
}
const next = () => go(current.value + 1);
const prev = () => go(current.value - 1);

function tick() {
  if (!hovering && !document.hidden) current.value = (current.value + 1) % count;
}
function restart() {
  if (timer === null) return;
  clearInterval(timer);
  timer = setInterval(tick, AUTOPLAY_MS);
}
function onKey(e) {
  if (e.key === "ArrowRight") { next(); e.preventDefault(); }
  else if (e.key === "ArrowLeft") { prev(); e.preventDefault(); }
}

/* 触屏滑动（轻量：30px 阈值判定方向） */
let touchX = null;
function onTouchStart(e) { touchX = e.changedTouches[0].clientX; }
function onTouchEnd(e) {
  if (touchX === null) return;
  const dx = e.changedTouches[0].clientX - touchX;
  if (Math.abs(dx) > 30) (dx < 0 ? next : prev)();
  touchX = null;
}

onMounted(() => {
  reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  if (!reducedMotion) timer = setInterval(tick, AUTOPLAY_MS);
});
onUnmounted(() => {
  if (timer !== null) clearInterval(timer);
  timer = null;
});
</script>

<template>
  <div
    class="showcase"
    aria-roledescription="轮播"
    aria-label="Falcon 界面展示"
    tabindex="0"
    @mouseenter="hovering = true"
    @mouseleave="hovering = false"
    @focusin="hovering = true"
    @focusout="hovering = false"
    @keydown="onKey"
    @touchstart.passive="onTouchStart"
    @touchend.passive="onTouchEnd"
  >
    <div class="frame">
      <div class="track" :style="trackStyle">
        <figure
          v-for="(s, i) in slides"
          :key="s.src"
          class="slide"
          :aria-hidden="i !== current"
          :aria-label="`${i + 1} / ${count}：${s.caption}`"
        >
          <!-- 全部 eager：7 张共 ~0.5MB，横向轨道里的 lazy 图片在被轮到之前
               不在视口附近，浏览器不会预取，自动播放时会白一拍 -->
          <img :src="s.src" :alt="s.alt" draggable="false" />
          <figcaption>
            <span class="tag" :class="s.tag === '设计原型' ? 'proto' : 'shot'">{{ s.tag }}</span>
            <span class="caption-text">{{ s.caption }}</span>
          </figcaption>
        </figure>
      </div>

      <button class="arrow left" aria-label="上一张" @click="prev">
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M15 18l-6-6 6-6"/></svg>
      </button>
      <button class="arrow right" aria-label="下一张" @click="next">
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M9 6l6 6-6 6"/></svg>
      </button>
    </div>

    <div class="dots" role="tablist" aria-label="选择幻灯片">
      <button
        v-for="(s, i) in slides"
        :key="`dot-${s.src}`"
        class="dot"
        :class="{ active: i === current }"
        role="tab"
        :aria-selected="i === current"
        :aria-label="`第 ${i + 1} 张：${s.caption}`"
        @click="go(i)"
      />
    </div>
  </div>
</template>

<style scoped>
.showcase {
  position: relative;
  margin: 8px 0 28px;
  outline: none;
}

.frame {
  position: relative;
  overflow: hidden;
  border: 1px solid var(--vp-c-divider);
  border-radius: 12px;
  background: var(--vp-c-bg-soft);
  box-shadow: 0 12px 32px -18px rgba(249, 115, 22, 0.25);
}

.track {
  display: flex;
  transition: transform 0.45s cubic-bezier(0.33, 1, 0.68, 1);
}

.slide {
  flex: 0 0 100%;
  min-width: 0;
  margin: 0;
}

.slide img {
  display: block;
  width: 100%;
  height: auto;
  user-select: none;
}

.slide figcaption {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 16px;
  border-top: 1px solid var(--vp-c-divider);
  font-size: 13px;
  line-height: 1.5;
  color: var(--vp-c-text-2);
}

.tag {
  flex: none;
  padding: 1px 8px;
  border-radius: 4px;
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.02em;
}
.tag.proto { color: #c2410c; background: rgba(249, 115, 22, 0.13); }
.tag.shot  { color: var(--vp-c-text-2); background: var(--vp-c-divider); }
:global(.dark) .tag.proto { color: #ffa07a; background: rgba(255, 160, 122, 0.15); }

.caption-text {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.arrow {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  display: flex;
  align-items: center;
  justify-content: center;
  width: 38px;
  height: 38px;
  padding: 0;
  border: 1px solid var(--vp-c-divider);
  border-radius: 50%;
  background: color-mix(in srgb, var(--vp-c-bg-elv) 82%, transparent);
  color: var(--vp-c-text-1);
  cursor: pointer;
  backdrop-filter: blur(6px);
  transition:
    color 0.2s,
    border-color 0.2s,
    transform 0.15s;
}
.arrow:hover {
  color: var(--vp-c-brand-1);
  border-color: rgba(249, 115, 22, 0.55);
}
.arrow:active { transform: translateY(-50%) scale(0.94); }
.arrow.left { left: 12px; }
.arrow.right { right: 12px; }

.dots {
  display: flex;
  justify-content: center;
  gap: 8px;
  padding-top: 12px;
}

.dot {
  width: 8px;
  height: 8px;
  padding: 0;
  border: none;
  border-radius: 4px;
  background: var(--vp-c-divider);
  cursor: pointer;
  transition:
    width 0.25s,
    background-color 0.25s;
}
.dot:hover { background: var(--vp-c-text-3); }
.dot.active {
  width: 22px;
  background: var(--vp-c-brand-1);
}

@media (max-width: 640px) {
  .arrow { width: 32px; height: 32px; }
  .caption-text { white-space: normal; }
  .slide figcaption { font-size: 12px; }
}

@media (prefers-reduced-motion: reduce) {
  .track { transition: none; }
}
</style>
