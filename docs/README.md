[中文](README.zh.md) | **English**

# Falcon Documentation

This directory builds the project documentation with **VitePress**; the source lives in `docs/src`.

> [!WARNING]
> `docs/src/` is the actively maintained source of truth for documentation.
> Most files at the root of `docs/*.md` are historical design drafts, migration notes, or archived material. They should no longer be treated as authoritative references for the current API, CLI, or build process.

See `docs/ARCHIVE.md` for an index of archived documents.

## Quick Start

### Install Dependencies

```bash
cd docs
pnpm install
```

### Development Mode

```bash
pnpm docs:dev
```

Open the local address printed in the terminal to view the docs.

### Build the Docs

```bash
pnpm docs:build
```

The build output is written to `docs/src/.vitepress/dist/`.

## Documentation Structure

```
docs/
├── package.json              # pnpm / Node config
├── README.md                 # This file
├── ARCHIVE.md                # Index of historical docs at the root
├── src/                      # VitePress source files
│   ├── index.md              # Home page
│   ├── .vitepress/           # VitePress configuration
│   │   └── config.mts        # Main config
│   ├── guide/                # User guide
│   │   ├── index.md          # Project introduction
│   │   ├── getting-started.md # Quick start
│   │   ├── installation.md    # Installation
│   │   └── usage.md          # Usage guide
│   ├── protocols/            # Protocol docs
│   │   ├── index.md          # Protocol overview
│   │   ├── http.md           # HTTP/HTTPS
│   │   ├── ftp.md            # FTP
│   │   ├── bittorrent.md     # BitTorrent
│   │   ├── private.md        # Private protocols
│   │   └── streaming.md      # HLS/DASH
│   ├── developer/            # Developer docs
│   │   ├── index.md          # Developer guide
│   │   ├── architecture.md   # Architecture
│   │   ├── coding-standards.md # Coding standards
│   │   ├── testing.md        # Testing guide
│   │   ├── plugins.md        # Plugin development
│   │   ├── debugging.md      # Debugging tips
│   │   └── release.md        # Release process
│   ├── api/                  # API reference
│   │   ├── index.md          # API overview
│   │   ├── core.md           # Core API
│   │   ├── plugins.md        # Plugin API
│   │   └── events.md         # Event API
│   └── faq.md                # FAQ
└── [Legacy docs retained at the root]
    ├── api_guide.md
    ├── developer_guide.md
    ├── protocols_guide.md
    └── ...
```

## VitePress Components

### Callouts

```markdown
::: tip Tip
This is a tip.
:::

::: warning Warning
This is a warning.
:::

::: danger Danger
This is a danger callout.
:::
```

### Code Blocks

```cpp
#include <iostream>
int main() {
    std::cout << "Hello, World!" << std::endl;
}
```

### Custom Containers

```md
::: details Click to view details
Hidden content
:::
```

## Deployment

### GitHub Pages

```bash
# Build
pnpm docs:build

# Push to the gh-pages branch
cd src/.vitepress/dist
git init
git add .
git commit -m "Deploy docs"
git push -f git@github.com:cuihairu/falcon.git master:gh-pages
```

### Vercel/Netlify

Connect the GitHub repository directly and set the build command to:

```bash
cd docs && pnpm install --frozen-lockfile && pnpm docs:build
```

Set the publish directory to `docs/src/.vitepress/dist`.

### GitHub Actions

The repository already includes a GitHub Pages deployment workflow; use it directly:

```bash
.github/workflows/docs-deploy.yml
```

Documentation validation workflow:

```bash
.github/workflows/docs-check.yml
```

Default behavior:

- Builds and deploys automatically on push to `main`
- Installs dependencies with `pnpm`
- Uploads `docs/src/.vitepress/dist` to GitHub Pages
- Sets the VitePress `base` automatically based on the repository name
- Runs documentation build validation independently on PR / push

## Learn More

- [VitePress documentation](https://vitepress.dev/)
