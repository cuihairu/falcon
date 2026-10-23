import { defineConfig } from "vitepress";

const repository = process.env.GITHUB_REPOSITORY ?? "";
const repositoryName = repository.split("/")[1] ?? "";
const isUserOrOrgPagesRepo = repositoryName.endsWith(".github.io");
const base =
  process.env.GITHUB_ACTIONS === "true" && repositoryName && !isUserOrOrgPagesRepo
    ? `/${repositoryName}/`
    : "/";

const englishNav = [
  { text: "Guide", link: "/guide/", activeMatch: "/guide/" },
  { text: "Protocols", link: "/protocols/", activeMatch: "/protocols/" },
  { text: "Developer", link: "/developer/", activeMatch: "/developer/" },
  { text: "API Reference", link: "/api/", activeMatch: "/api/" },
  { text: "FAQ", link: "/faq", activeMatch: "^/faq" },
];

const chineseNav = [
  { text: "指南", link: "/zh/guide/", activeMatch: "/zh/guide/" },
  { text: "协议", link: "/zh/protocols/", activeMatch: "/zh/protocols/" },
  { text: "开发者", link: "/zh/developer/", activeMatch: "/zh/developer/" },
  { text: "API 参考", link: "/zh/api/", activeMatch: "/zh/api/" },
  { text: "常见问题", link: "/zh/faq", activeMatch: "^/zh/faq" },
];

const englishSidebar = {
  "/guide/": [
    {
      text: "Getting Started",
      items: [
        { text: "Introduction", link: "/guide/" },
        { text: "Quick Start", link: "/guide/getting-started" },
        { text: "Installation", link: "/guide/installation" },
        { text: "Usage", link: "/guide/usage" },
      ],
    },
  ],
  "/protocols/": [
    {
      text: "Protocol Support",
      items: [
        { text: "Overview", link: "/protocols/" },
        { text: "HTTP/HTTPS", link: "/protocols/http" },
        { text: "FTP", link: "/protocols/ftp" },
        { text: "BitTorrent", link: "/protocols/bittorrent" },
        { text: "Private Protocols", link: "/protocols/private" },
        { text: "Streaming", link: "/protocols/streaming" },
      ],
    },
  ],
  "/developer/": [
    {
      text: "Developer Guide",
      items: [
        { text: "Overview", link: "/developer/" },
        { text: "Architecture", link: "/developer/architecture" },
        { text: "Coding Standards", link: "/developer/coding-standards" },
        { text: "Testing", link: "/developer/testing" },
        { text: "Plugin Development", link: "/developer/plugins" },
        { text: "Desktop Redesign", link: "/developer/desktop-redesign" },
        { text: "UI Design Prototypes", link: "/developer/design-prototypes" },
        { text: "Debugging", link: "/developer/debugging" },
        { text: "Release Process", link: "/developer/release" },
      ],
    },
  ],
  "/api/": [
    {
      text: "API Reference",
      items: [
        { text: "Overview", link: "/api/" },
        { text: "Core API", link: "/api/core" },
        { text: "Plugin API", link: "/api/plugins" },
        { text: "Events API", link: "/api/events" },
      ],
    },
  ],
};

const chineseSidebar = {
  "/zh/guide/": [
    {
      text: "入门",
      items: [
        { text: "介绍", link: "/zh/guide/" },
        { text: "快速开始", link: "/zh/guide/getting-started" },
        { text: "安装", link: "/zh/guide/installation" },
        { text: "使用", link: "/zh/guide/usage" },
      ],
    },
  ],
  "/zh/protocols/": [
    {
      text: "协议支持",
      items: [
        { text: "概览", link: "/zh/protocols/" },
        { text: "HTTP/HTTPS", link: "/zh/protocols/http" },
        { text: "FTP", link: "/zh/protocols/ftp" },
        { text: "BitTorrent", link: "/zh/protocols/bittorrent" },
        { text: "私有协议", link: "/zh/protocols/private" },
        { text: "流媒体", link: "/zh/protocols/streaming" },
      ],
    },
  ],
  "/zh/developer/": [
    {
      text: "开发者指南",
      items: [
        { text: "概览", link: "/zh/developer/" },
        { text: "架构", link: "/zh/developer/architecture" },
        { text: "编码规范", link: "/zh/developer/coding-standards" },
        { text: "测试", link: "/zh/developer/testing" },
        { text: "插件开发", link: "/zh/developer/plugins" },
        { text: "桌面端重设计", link: "/zh/developer/desktop-redesign" },
        { text: "UI 设计原型", link: "/zh/developer/design-prototypes" },
        { text: "调试", link: "/zh/developer/debugging" },
        { text: "发布流程", link: "/zh/developer/release" },
      ],
    },
  ],
  "/zh/api/": [
    {
      text: "API 参考",
      items: [
        { text: "概览", link: "/zh/api/" },
        { text: "核心 API", link: "/zh/api/core" },
        { text: "插件 API", link: "/zh/api/plugins" },
        { text: "事件 API", link: "/zh/api/events" },
      ],
    },
  ],
};

export default defineConfig({
  title: "Falcon Downloader",
  description: "A modern, cross-platform C++ download solution",
  base,
  cleanUrls: true,
  lastUpdated: true,
  outDir: ".vitepress/dist",
  markdown: {
    lineNumbers: true,
    theme: { light: "github-light", dark: "github-dark" },
  },
  locales: {
    root: {
      label: "English",
      lang: "en-US",
      title: "Falcon Downloader",
      description: "A modern, cross-platform C++ download solution",
      themeConfig: {
        nav: englishNav,
        sidebar: englishSidebar,
        footer: {
          message: "Released under the Apache 2.0 License",
          copyright: "Copyright © 2025-present Falcon Team",
        },
        outline: { level: [2, 3], label: "On this page" },
        docFooter: { prev: "Previous page", next: "Next page" },
        lastUpdated: { text: "Last updated" },
      },
    },
    zh: {
      label: "简体中文",
      lang: "zh-CN",
      link: "/zh/",
      title: "Falcon 下载器",
      description: "现代化、跨平台的 C++ 下载解决方案",
      themeConfig: {
        nav: chineseNav,
        sidebar: chineseSidebar,
        footer: {
          message: "基于 Apache 2.0 许可证发布",
          copyright: "Copyright © 2025-present Falcon Team",
        },
        outline: { level: [2, 3], label: "本页目录" },
        docFooter: { prev: "上一页", next: "下一页" },
        lastUpdated: { text: "最后更新" },
      },
    },
  },
  themeConfig: {
    siteTitle: "Falcon Downloader",
    logo: "/logo.svg",
    editLink: {
      pattern: "https://github.com/cuihairu/falcon/edit/main/docs/src/:path",
      text: "Edit this page on GitHub",
    },
    socialLinks: [
      { icon: "github", link: "https://github.com/cuihairu/falcon" },
    ],
    search: {
      provider: "local",
    },
  },
  head: [
    ["link", { rel: "icon", type: "image/svg+xml", href: `${base}favicon.svg` }],
    ["meta", { name: "theme-color", content: "#f97316" }],
    ["meta", { property: "og:type", content: "website" }],
    ["meta", { property: "og:locale", content: "en" }],
    ["meta", { property: "og:title", content: "Falcon Downloader" }],
    ["meta", { property: "og:site_name", content: "Falcon Downloader" }],
    [
      "meta",
      {
        property: "og:description",
        content: "A modern, cross-platform C++ download solution",
      },
    ],
  ],
});
