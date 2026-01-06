import { defineUserConfig } from "vuepress";
import { viteBundler } from "@vuepress/bundler-vite";
import { getDirname, path } from "@vuepress/utils";
import theme from "./theme.js";

const __dirname = getDirname(import.meta.url);

export default defineUserConfig({
  bundler: viteBundler({
    viteOptions: {},
    vuePluginOptions: {},
  }),

  lang: "zh-CN",
  title: "Falcon 下载器",
  description: "现代化、跨平台的 C++ 下载解决方案",
  base: "/",

  head: [
    ["link", { rel: "icon", href: "/logo.png" }],
    ["meta", { name: "theme-color", content: "#3c8772" }],
    ["meta", { property: "og:type", content: "website" }],
    ["meta", { property: "og:locale", content: "zh-CN" }],
    ["meta", { property: "og:title", content: "Falcon 下载器" }],
    ["meta", { property: "og:site_name", content: "Falcon 下载器" }],
    [
      "meta",
      {
        property: "og:description",
        content: "现代化、跨平台的 C++ 下载解决方案",
      },
    ],
  ],

  theme,

  // 生成源文件的别名
  alias: {
    "@": path.resolve(__dirname, "./"),
  },

  // 构建相关
  dest: "./dist",

  // 插件
  plugins: [],
});
