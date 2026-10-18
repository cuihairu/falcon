import { defineConfig } from "vitepress";

const repository = process.env.GITHUB_REPOSITORY ?? "";
const repositoryName = repository.split("/")[1] ?? "";
const isUserOrOrgPagesRepo = repositoryName.endsWith(".github.io");
const base =
  process.env.GITHUB_ACTIONS === "true" && repositoryName && !isUserOrOrgPagesRepo
    ? `/${repositoryName}/`
    : "/";

export default defineConfig({
  lang: "en",
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
  themeConfig: {
    siteTitle: "Falcon Downloader",
    logo: "/logo.svg",
    editLink: {
      pattern: "https://github.com/cuihairu/falcon/edit/main/docs/src/:path",
      text: "Edit this page on GitHub",
    },
    nav: [
      { text: "Guide", link: "/guide/", activeMatch: "/guide/" },
      { text: "Protocols", link: "/protocols/", activeMatch: "/protocols/" },
      { text: "Developer", link: "/developer/", activeMatch: "/developer/" },
      { text: "API Reference", link: "/api/", activeMatch: "/api/" },
      { text: "FAQ", link: "/faq", activeMatch: "^/faq" },
    ],
    sidebar: {
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
    },
    socialLinks: [
      { icon: "github", link: "https://github.com/cuihairu/falcon" },
    ],
    footer: {
      message: "Released under the Apache 2.0 License",
      copyright: "Copyright © 2025-present Falcon Team",
    },
    outline: {
      level: [2, 3],
      label: "On this page",
    },
    search: {
      provider: "local",
    },
    docFooter: {
      prev: "Previous page",
      next: "Next page",
    },
    lastUpdated: {
      text: "Last updated",
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
