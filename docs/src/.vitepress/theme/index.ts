import DefaultTheme from "vitepress/theme";
import "@fontsource-variable/geist";
import "@fontsource-variable/geist-mono";
import "./custom.css";
import ShowcaseCarousel from "./ShowcaseCarousel.vue";

export default {
  extends: DefaultTheme,
  enhanceApp({ app }: { app: any }) {
    // 首页界面走马灯（设计原型 + 实况截图轮播）
    app.component("ShowcaseCarousel", ShowcaseCarousel);
  },
};
