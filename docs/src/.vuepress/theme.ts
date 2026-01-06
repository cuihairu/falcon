import { defaultTheme } from "@vuepress/theme-default";
import { navbar } from "./navbar.js";
import { sidebar } from "./sidebar.js";

export default defaultTheme({
  logo: "/logo.png",

  // 导航栏
  navbar,

  // 侧边栏
  sidebar,

  // 页脚
  footer: {
    message: "基于 Apache 2.0 许可发布",
    copyright: "Copyright © 2025-present Falcon Team",
  },

  // 编辑链接
  editLink: false,

  // 最后更新时间
  lastUpdated: true,
  lastUpdatedText: "最后更新",

  // 贡献者
  contributors: false,

  // 代码块主题
  tip: "提示",
  warning: "注意",
  danger: "警告",

  // 主题色
  themeColor: {
    light: "#3c8772",
    dark: "#3c8772",
  },

  // 语言下拉
  locales: {},
});
