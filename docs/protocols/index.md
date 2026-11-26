# Protocol Support

Falcon organizes transfer capabilities in layers around one engine: core transfer protocols are enabled by default, P2P and multi-source capabilities run on the same engine, private protocol families are optional compatibility plugins, and object storage plus resource browsing are provided by the storage package. The status below reflects the current codebase and top-level build configuration (`FALCON_ENABLE_*` in `CMakeLists.txt`).

## Supported protocols

| Layer | Protocol | Default | Notes |
|------|------|------|------|
| **1 · Core transfer** | HTTP/HTTPS | On | V1/libcurl and V2 event-driven engines, resume, segmented downloads, proxies |
| **1 · Core transfer** | FTP/FTPS | On | Active/passive modes, REST resume |
| **1 · Core transfer** | SFTP | On (requires libssh2) | known_hosts verification, resume |
| **1 · Core transfer** | WebDAV | On | `dav`/`davs`; the storage layer also provides a WebDAV browser |
| **2 · P2P and multi-source** | BitTorrent/Magnet | On (requires libtorrent) | Piece-level P2P, seeding, DHT, NAT port mapping |
| **2 · P2P and multi-source** | Metalink | On | RFC 5854 `.meta4` / Metalink3 `.metalink`, multi-mirror P2SP segmentation with pre-publish hash verification |
| **3 · Compatibility plugins** | Thunder / QQDL / FlashGet / ED2K / HLS-DASH | Optional | Off by default; enable through `FALCON_ENABLE_*` as needed |
| **Resource access** | S3 / OSS / COS / Kodo / Upyun | On | Object storage (SigV4), MinIO / RustFS private gateways, remote browsing, and resource search |

## Basic protocols

### HTTP/HTTPS

The most common download protocol, with comprehensive Falcon support:

- Resume
- Multi-threaded segmented downloads
- Speed limiting
- Custom HTTP headers
- Proxy support
- Authentication support

[Read the full documentation →](./http.md)

### FTP/FTPS

Traditional file transfer:

- Active and passive modes
- Anonymous and authenticated login
- FTPS (explicit TLS) support

[Read the full documentation →](./ftp.md)

## P2P protocols

### BitTorrent

BitTorrent is a P2P capability enabled by default, with libtorrent handling the data plane:

- Magnet links and `.torrent` files
- DHT (native libtorrent), LSD, UPnP, NAT-PMP
- Optional file selection, seeding (seed-ratio / seed-time)
- Graceful fallback to a pure-C++ experimental mode without libtorrent

[Read the full documentation →](./bittorrent.md)

### ED2K

The codebase contains ED2K-related implementations, but the default build normally disables them:

- File-link support
- Server connections
- Source exchange
- AICH verification

## Private protocols

### Thunder

The codebase retains private-protocol implementations:

- Classic `thunder://` links
- Offline `thunderxl://` links

[Read the full documentation →](./private.md)

### QQDL (QQ Xuanfeng)

Tencent's download protocol:

- Standard `qqlink://` links
- Links with a GID

### FlashGet

The legacy downloader protocol:

- Full `flashget://` format
- Short `fg://` format

## Streaming protocols

### HLS/DASH

The codebase retains HLS/DASH-related implementations:

- `.m3u8` playlist parsing
- `.mpd` manifest recognition
- Segment-by-segment download and merge
- Relative-address resolution inside playlists

[Read the full documentation →](./streaming.md)

## Enabling protocols at build time

Control compiled protocols with CMake options (the first six default to ON and the last five to OFF):

```bash
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_SFTP=ON \
  -DFALCON_ENABLE_WEBDAV=ON \
  -DFALCON_ENABLE_METALINK=ON \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON
```

> Missing dependencies degrade gracefully: BitTorrent without libtorrent and SFTP without libssh2 are disabled automatically with a WARNING. HTTP/FTP/WebDAV are provided through libcurl.

## Checking supported protocols

The most reliable current method is to inspect the `FALCON_ENABLE_*` options passed to the build and whether the corresponding plugin was enabled in the configuration log.

## Protocol comparison

| Protocol | Resource usage | Typical scenario |
|------|----------|----------|
| HTTP/HTTPS | Low | Web and file downloads |
| FTP | Low | Enterprise file sharing |
| BitTorrent | High | Large-file distribution |
| ED2K | Medium | Legacy resource sharing |
| Thunder/FlashGet | Medium | Chinese-language resources |
| HLS/DASH | Medium | Video on demand |

Speed and stability depend on the origin and network conditions; the protocols themselves do not support a single universal score, so there is no star-rating column.

::: tip Choosing a protocol
- **Everyday downloads**: use HTTP/HTTPS.
- **Large files**: use BitTorrent.
- **Chinese-language resources**: use Thunder links.
- **Video**: use HLS/DASH.
:::
