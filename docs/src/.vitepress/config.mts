import { defineConfig } from "vitepress";

const repository = process.env.GITHUB_REPOSITORY ?? "";
const repositoryName = repository.split("/")[1] ?? "";
const isUserOrOrgPagesRepo = repositoryName.endsWith(".github.io");
const base =
  process.env.GITHUB_ACTIONS === "true" && repositoryName && !isUserOrOrgPagesRepo
    ? `/${repositoryName}/`
    : "/";

export default defineConfig({
  lang: "zh-CN",
  title: "Falcon 下载器",
  description: "现代化、跨平台的 C++ 下载解决方案",
  base,
  cleanUrls: true,
  lastUpdated: true,
  outDir: ".vitepress/dist",
  themeConfig: {
    siteTitle: "Falcon 下载器",
    logo: "/favicon.svg",
    nav: [
      { text: "指南", link: "/guide/" },
      { text: "协议支持", link: "/protocols/" },
      { text: "开发者", link: "/developer/" },
      { text: "API 参考", link: "/api/" },
      { text: "常见问题", link: "/faq" },
    ],
    sidebar: {
      "/guide/": [
        {
          text: "入门指南",
          items: [
            { text: "项目介绍", link: "/guide/" },
            { text: "快速开始", link: "/guide/getting-started" },
            { text: "安装说明", link: "/guide/installation" },
            { text: "使用指南", link: "/guide/usage" },
          ],
        },
      ],
      "/protocols/": [
        {
          text: "协议支持",
          items: [
            { text: "概览", link: "/protocols/" },
            { text: "HTTP/HTTPS", link: "/protocols/http" },
            { text: "FTP", link: "/protocols/ftp" },
            { text: "BitTorrent", link: "/protocols/bittorrent" },
            { text: "私有协议", link: "/protocols/private" },
            { text: "流媒体协议", link: "/protocols/streaming" },
          ],
        },
      ],
      "/developer/": [
        {
          text: "开发者指南",
          items: [
            { text: "概览", link: "/developer/" },
            { text: "架构设计", link: "/developer/architecture" },
            { text: "编码规范", link: "/developer/coding-standards" },
            { text: "测试指南", link: "/developer/testing" },
            { text: "插件开发", link: "/developer/plugins" },
            { text: "调试技巧", link: "/developer/debugging" },
            { text: "发布流程", link: "/developer/release" },
          ],
        },
      ],
      "/api/": [
        {
          text: "API 参考",
          items: [
            { text: "概览", link: "/api/" },
            { text: "核心 API", link: "/api/core" },
            { text: "插件 API", link: "/api/plugins" },
            { text: "事件 API", link: "/api/events" },
          ],
        },
      ],
    },
    socialLinks: [
      { icon: "github", link: "https://github.com/cuihairu/falcon" },
    ],
    footer: {
      message: "基于 Apache 2.0 许可发布",
      copyright: "Copyright © 2025-present Falcon Team",
    },
    outline: {
      level: [2, 3],
      label: "页面导航",
    },
    search: {
      provider: "local",
    },
    docFooter: {
      prev: "上一页",
      next: "下一页",
    },
    lastUpdated: {
      text: "最后更新于",
    },
  },
  head: [
    ["link", { rel: "icon", type: "image/svg+xml", href: `${base}favicon.svg` }],
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
});
