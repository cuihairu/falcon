[中文](protocol_support.zh.md) | **English**

# Falcon Downloader Protocol Support

> [!NOTE]
> This document is a historical overview; for the current protocol support status, refer to `docs/src/protocols/README.md`.

## Implemented Protocols and Features

### 1. Core Protocols

#### HTTP/HTTPS
- **File location**: `plugins/http/http_plugin.cpp`
- **Features**:
  - Basic GET/POST requests
  - Resumable downloads (Range requests)
  - Multi-threaded chunked downloads
  - Speed control
  - Custom request headers
  - Redirect following
  - Proxy support (HTTP/SOCKS4/SOCKS5)
  - SSL certificate verification control

#### FTP
- **File location**: `plugins/ftp/ftp_plugin.cpp`
- **Features**:
  - Passive/active mode
  - Anonymous and authenticated login
  - Resumable downloads (REST command)
  - Directory listing
  - File information retrieval

### 2. Proprietary Protocols and Magnet Links

#### Thunder
- **File location**: `plugins/thunder/thunder_plugin.cpp`
- **Supported formats**:
  - `thunder://` (standard format)
  - `thunderxl://` (Thunder Express)
- **Implementation**: Base64 decoding back to an HTTP link

#### QQDL (QQ Whirlwind)
- **File location**: `plugins/qqdl/qqdl_plugin.cpp`
- **Supported format**: `qqlink://`
- **Implementation**: GID validation + Base64 decoding

#### FlashGet
- **File location**: `plugins/flashget/flashget_plugin.cpp`
- **Supported format**: `flashget://`
- **Implementation**: URL decoding + parameter extraction

#### ED2K (eDonkey)
- **File location**: `plugins/ed2k/ed2k_plugin.cpp`
- **Supported format**: `ed2k://`
- **Implementation**: MD4 hash parsing + eD2k link handling

#### BitTorrent/Magnet
- **File location**: `plugins/bittorrent/bittorrent_plugin.cpp`
- **Supported formats**:
  - Magnet links `magnet:?xt=urn:btih:`
  - Torrent files `.torrent`
- **Features**: DHT network support, tracker support, piece verification

#### HLS/DASH
- **File location**: `plugins/hls/hls_plugin.cpp`
- **Supported formats**:
  - M3U8 playlists (HLS)
  - MPD manifests (MPEG-DASH)
- **Features**: Adaptive bitrate streaming, multi-resolution selection

### 3. Cloud Drives and Cloud Storage

#### Lanzou Cloud
- **File location**: `src/cloud_storage_plugin.cpp`
- **Supported formats**: `lanzouy.com`, `lanzoux.com`, etc.
- **Features**:
  - Share link parsing
  - Password extraction support
  - File information retrieval
  - Direct link downloads

#### Cloud Drive Support Matrix (Native WebDAV / Alist Aggregation)
- **File location**: `packages/libfalcon-storage/plugins/webdav/webdav_browser.cpp` (browsing/metadata), `include/falcon/storage/storage_presets.hpp` (connection presets)
- **Supported format**: `webdav://[user:pass@]host[:port]/path` (`dav://`, `davs://`, and `webdavs://` are synonyms)
- **Features**:
  - PROPFIND browsing (Depth 0/1, 207 multistatus parsing, namespace-prefix tolerance)
  - MKCOL / DELETE (recursive Depth: infinity + per-item fallback) / MOVE / COPY (Destination)
  - Authentication negotiated by curl via 401 challenges (Basic/Digest)
  - RFC 4331 quotas (returns empty when the server does not support them)
  - href percent-decoding, per-segment path encoding, recursive flattened listings
- **Cloud drive catalog**:

| Cloud drive | Carrier | Preset id | Notes |
|------|----------|---------|------|
| Google Drive | Alist aggregation | `google-drive` | OAuth handled on the Alist side |
| OneDrive | Alist aggregation | `onedrive` | Personal/business editions |
| Dropbox | Alist aggregation | `dropbox` | |
| pCloud | Alist aggregation | `pcloud` | |
| MEGA | Alist aggregation | `mega` | Credentials configured in the Alist storage driver |
| Jianguoyun (Nutstore) | Native WebDAV | `jianguoyun` | `https://dav.jianguoyun.com/dav`, app password |
| Aliyun Drive | Alist aggregation | `aliyundrive` | Existing domestic matrix |
| Baidu Netdisk | Alist aggregation | `baidu` | Existing domestic matrix |
| Tianyi Cloud Drive | Alist aggregation | `tianyi` | Existing domestic matrix |
| 115 Drive | Alist aggregation | `115` | Existing domestic matrix |
| Generic WebDAV | Native | `webdav` | NAS/InfiniCloud/self-hosted |
| Alist endpoint | Native | `alist` | Any Alist instance |

- **Data plane boundary**: The actual download of cloud drive files (the data plane) is handled by the protocol-layer WebDAV plugin, with URLs of the form `dav(s)://host[:port]/dav/path`; browsing, create/delete, rename, move, and quota surfaces are all covered by this browser.
- **Verification boundary**: Mock HTTP tests cover every request/parsing path (401 Basic challenge, namespace casing, absolute-URL href, recursive fallback deletion, etc. — 25 cases); for live checks against the Jianguoyun endpoint and an Alist container, see "Live Connectivity Walkthrough" below.

### 4. Object Storage

#### S3-Compatible Object Storage Matrix
- **File location**: `packages/libfalcon-storage/plugins/s3/s3_browser.cpp`, `include/falcon/storage/storage_presets.hpp`
- **Protocol surface**: ListObjectsV2 XML (`list-type=2`) — real S3/MinIO/R2/B2/Wasabi/GCS all respond with XML `ListBucketResult`, parsed by the hand-written `xml_scan.hpp` scanner (zero new dependencies, namespace-tolerant + five predefined entities decoded); the legacy JSON form is retained as a compatibility path
- **Vendor matrix**:

| Vendor | Preset id | endpoint | Auth | Notes |
|------|---------|----------|------|------|
| Amazon S3 | `aws-s3` | Assembled automatically from bucket+region | SigV4 | `region` defaults to us-east-1 |
| Cloudflare R2 | `cloudflare-r2` | `https://<account_id>.r2.cloudflarestorage.com` | SigV4 | Get the account address from the R2 page in the console |
| Backblaze B2 | `backblaze-b2` | `https://s3.<region>.backblazeb2.com` | SigV4 | Region such as us-west-004 |
| Wasabi | `wasabi` | `https://s3.<region>.wasabisys.com` | SigV4 | |
| Google Cloud Storage | `google-cloud-storage` | `https://storage.googleapis.com` | SigV4 + HMAC | HMAC keys are created in interoperability settings |
| MinIO / self-hosted S3-compatible | `minio` | User-supplied (including port) | SigV4 | MinIO/RustFS/SeaweedFS/Ceph RGW |
| Azure Blob | `azure-blob` | Via Alist | Alist account | Azure has no S3-compatible protocol; goes through the cloud drive carrier |

- **Domestic cloud vendors (existing matrix, presets retained)**: Alibaba OSS (`oss`), Tencent COS (`cos`), Qiniu Kodo (`kodo`), Upyun (`upyun`)
- **Verification boundary**: This batch fixed and live-tested the S3 browsing surface's XML parsing and delimiter directory semantics (RustFS real stack, 12/12 passing — previously, without XML parsing, list always returned empty); the OSS/COS/Kodo/Upyun browsers still use the JSON parsing path, which does not match the real XML surface (the same gap, not fixed in this batch, and no connectivity walkthrough done) — the four domestic vendors are bounded by protocol-compatible implementation + mock tests.

#### Live Connectivity Walkthrough (2026-10-02, docker real stacks)
- **RustFS** (docker container, real S3-compatible surface, strict SigV4 verification): connect probe / delimiter directory semantics (nested objects no longer leak into the root layer) / file size and metadata / directory creation / directory rename (copy with trailing-slash marker fallback) / delete-marker cleanup — 12/12 passing. Two real-surface defects were found and fixed during the walkthrough: ① without a delimiter on non-list requests, nested objects leaked flat as root-level files; ② PUT copying a directory marker returned 404 (NoSuchKey), and DELETE silently missed deleting the trailing-slash marker. Note: MinIO officially pulled its Docker Hub image (and quay was blocked by the local network), so RustFS was chosen as the S3-compatible real stack — same protocol surface, with stricter SigV4 verification than MinIO.
- **Alist** (docker container, WebDAV endpoint, Local driver mount): PROPFIND probe/browse / MKCOL / MOVE / recursive DELETE all passed; **COPY (collections and files) returns 500 from Alist** — a limitation of its WebDAV COPY implementation (unrelated to our side), recorded as-is; quota fields are not returned (RFC 4331 absent → empty map, per the agreed behavior).
- **rclone serve webdav** (docker container, a compliant real WebDAV surface): PROPFIND / file COPY (201) / DELETE all passed — COPY semantics verified against a compliant server.
- **Jianguoyun / AWS / R2 / B2 / Wasabi / GCS**: no online accounts; covered by protocol-compatible implementation + mock test stubs (XML forms consistent with each vendor's documentation), no live testing — boundary noted as-is.

### 5. Resource Search

#### Search Engine Integration
- **File location**: `src/resource_search.cpp`
- **Supported search engines**:
  - TorrentGalaxy
  - 1337x
  - ThePirateBay
  - EZTV
  - MagnetDL
- **Features**:
  - Concurrent multi-engine search
  - Result deduplication and ranking
  - Smart filtering (size, seed count, etc.)
  - Cloud drive resource search support
  - JSON configuration file support

### 6. NAS Protocol Support Plans (Not Implemented)

#### Samba/CIFS (not implemented)
- **Protocol**: Windows network share protocol
- **Implementation plan**:
  - Use the libsmbclient library
  - Support SMB 2.0/3.0
  - Authentication support (username/password, Kerberos)
  - File browsing and download
- **Estimated delivery**: Q1 2024

#### NFS (not implemented)
- **Protocol**: Network File System protocol
- **Implementation plan**:
  - Use the libnfs library
  - NFSv3/NFSv4 support
  - Auto-mount capability
  - Large file transfer optimization
- **Estimated delivery**: Q1 2024

#### WebDAV (implemented)
- **Protocol**: HTTP-based distributed file system
- **Status**: Implemented — `dav://` / `davs://` / `webdavs://` entry points, libcurl data plane, PROPFIND browsing, Basic/Digest authentication, resumable downloads; for remote directory browsing see the webdav_browser in `packages/libfalcon-storage` (the "Cloud Drive Support Matrix" above)

#### FTPS/SFTP (implemented)
- **Protocol**: Secure file transfer protocols
- **Status**: SFTP is implemented (libssh2 data plane, known_hosts host key verification, public-key/password authentication, resumable downloads); FTPS is supported via libcurl (explicit/tls)

### 7. Advanced Features

#### Proxy Support
- **Supported types**:
  - HTTP proxy
  - SOCKS4 proxy
  - SOCKS5 proxy
  - Proxy authentication (username/password)
- **Configuration**: command-line arguments, configuration files

#### Resource Search and Cloud Drive Integration
- **Description**:
  - Search results include cloud drive share links
  - Automatic detection and parsing of cloud drive links
  - A unified download experience

### Usage Examples

```bash
# HTTP download
falcon-cli https://example.com/file.zip

# Magnet link download
falcon-cli "magnet:?xt=urn:btih:..."

# Download through a proxy
falcon-cli https://example.com/large.iso --proxy socks5://127.0.0.1:1080
```

The `--search` / `--min-seeds` / `--download` / `--password` arguments and the `s3://` entry point mentioned in the original snapshot are not implemented: cloud storage browsing and resource search are currently library-level capabilities (used by the desktop cloud drive page), and the CLI provides no entry point for them. For the full argument list, refer to `falcon-cli --help`.

## Architecture Design

### Plugin Architecture
- All protocols are implemented through the unified `IProtocolHandler` interface
- Dynamic plugin loading and unloading
- Configurable feature toggles

### Core Components
1. **DownloadEngine**: the download engine core
2. **TaskManager**: task management and scheduling
3. **PluginManager**: plugin management and routing
4. **ResourceSearchManager**: resource search management
5. **CloudStorageManager**: cloud drive storage management

### Dependencies
- **Required**: libcurl (HTTP requests)
- **Optional**:
  - nlohmann/json (JSON parsing)
  - spdlog (logging)
  - libtorrent (BitTorrent)
  - OpenSSL (S3 signing)
  - libssh2 (SFTP)
  - libsmbclient (Samba, not implemented)

## Future Plans

The following is the planning list as of the snapshot time, archived only: the desktop GUI (Qt6), WebDAV, SFTP, and P2P acceleration (multi-source segmentation) have since landed; the rest are not implemented. For the current state, refer to the [README](../README.md).

### Short-term goals (3 months)
1. Complete NAS protocol support (Samba, NFS, WebDAV)
2. Improve search performance and accuracy
3. Add support for more cloud drive platforms
4. Improve error handling and retry mechanisms

### Mid-term goals (6 months)
1. Implement P2P network acceleration
2. Add direct video streaming playback support
3. Implement task scheduling and priority management
4. Develop a GUI

### Long-term goals (1 year)
1. Support more professional storage systems
2. Build a distributed download network
3. Add AI-assisted resource recommendations
4. Enterprise-grade features and management interface

---

**Last updated**: 2026-10-02
**Version**: 0.2.0
