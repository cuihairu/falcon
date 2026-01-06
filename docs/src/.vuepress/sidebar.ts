export const sidebar = {
  "/guide/": [
    {
      text: "入门指南",
      children: [
        "/guide/README.md",
        "/guide/getting-started.md",
        "/guide/installation.md",
        "/guide/usage.md",
      ],
    },
    {
      text: "配置",
      children: [
        "/guide/configuration.md",
        "/guide/daemon.md",
      ],
    },
  ],

  "/protocols/": [
    {
      text: "协议支持",
      children: [
        "/protocols/README.md",
        "/protocols/http.md",
        "/protocols/ftp.md",
        "/protocols/bittorrent.md",
        "/protocols/private.md",
        "/protocols/streaming.md",
      ],
    },
  ],

  "/developer/": [
    {
      text: "开发者指南",
      children: [
        "/developer/README.md",
        "/developer/architecture.md",
        "/developer/coding-standards.md",
        "/developer/testing.md",
        "/developer/plugins.md",
        "/developer/debugging.md",
        "/developer/release.md",
      ],
    },
  ],

  "/api/": [
    {
      text: "API 参考",
      children: [
        "/api/README.md",
        "/api/core.md",
        "/api/plugins.md",
        "/api/events.md",
      ],
    },
  ],
};
