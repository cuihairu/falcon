[中文](private_protocols.zh.md) | **English**

# Falcon Private Protocol Support

> [!NOTE]
> This document is oriented toward design and background; for currently available information, refer first to `docs/src/protocols/private.md`.

This document describes Falcon's support for various private download protocols.

## Supported Protocols

### 1. Thunder Protocol

**Protocol identifiers**: `thunder://` and `thunderxl://`

**Description**: Thunder is a widely used download tool in China that employs a special encoding scheme for sharing download links.

**Link format**:
- Classic format: `thunder://[Base64-encoded content]`
- Offline format: `thunderxl://[Base64-encoded content]`

**Implementation details**:
- Base64 decoding
- Removal of AA/ZZ markers (classic format)
- Automatic conversion to the original HTTP/FTP link

**Example**:
```cpp
// Built-in protocols are registered automatically when the DownloadEngine
// is constructed; no manual plugin setup is needed
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

// Download a Thunder link
std::string url = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg==";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 2. QQDL Protocol

**Protocol identifiers**: `qqlink://` and `qqdl://`

**Description**: QQDL is a download tool from Tencent that supports sharing resources through special links.

**Link format**:
- GID format: `qqlink://[GID]|[URL]` (pipe-separated; the second field after the GID is the target URL)
- Base64 format: `qqlink://[Base64-encoded URL]`

**Implementation details**:
- GID prefix field skipped; the URL after it is extracted
- Base64 fallback: the decoded result must start with http/https/ftp
- Multi-field `|` separated forms supported (first field containing `://` wins)

**Example**:
```cpp
// Built-in protocols register automatically with the engine
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

std::string url = "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 3. FlashGet Protocol

**Protocol identifiers**: `flashget://` and `fg://`

**Description**: FlashGet is a veteran download manager.

**Link format**:
- Full format: `flashget://[Base64 payload]&ref=[referring page]` (the `&ref=` suffix is stripped; the decoded payload drops the `[FLASHGET]` prefix)
- Short format: `fg://[URL-encoded URL]` (URL-encoded, not Base64)
- Base64 shorthand: `flashget://[Base64 payload]` (works without the `&ref=` suffix too)

**Implementation details**:
- URL decoding / Base64 decoding dispatched by payload shape
- `[FLASHGET]` prefix removal (inside the decoded payload)
- Automatic conversion to a standard HTTP download

**Example**:
```cpp
// Built-in protocols register automatically with the engine
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

std::string url = "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA==";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 4. ED2K Protocol (eDonkey2000)

**Protocol identifier**: `ed2k://`

**Description**: ED2K is the protocol used by the eDonkey2000 network, also known as the eDonkey protocol, for P2P file sharing.

**Link format**:
- File link: `ed2k://file|[filename]|[file size]|[MD4 hash]|/` (optional `|h=AICH hash` segment)
- Server links `ed2k://server|...` are not supported; parsing them fails with an error

**Implementation details**:
- File-link field parsing (name/size/hash; fewer than 4 fields is an error)
- AICH (Advanced Intelligent Corruption Handling) hash segment recognition
- Multi-source delegated download

**Example**:
```cpp
// Built-in protocols register automatically with the engine
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

std::string url = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 5. HLS/DASH Streaming Protocols

**Protocol identification**: URLs ending in `.m3u8` route to the HLS handler (`.mpd` is not implemented yet)

**Description**: HTTP Live Streaming (HLS) is a commonly used streaming protocol; DASH manifest parsing is not implemented yet.

**HLS format**: `.m3u8` playlist file

**Implementation details**:
- Playlist parsing (EXTINF duration info)
- Media segments downloaded in order and joined into one binary file
- Master-playlist stream selection and encrypted streams are not supported yet

**Example**:
```cpp
// Built-in protocols register automatically with the engine
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

// Download an HLS stream
std::string hlsUrl = "https://example.com/playlist.m3u8";
auto task = engine.add_task(hlsUrl, options);
engine.start_task(task->id());
```

## Build Configuration

Use CMake options to enable or disable specific protocols (all five private protocols default to OFF):

```bash
# Enable all private protocols
cmake -B build \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON

# Enable only the protocols you need
cmake -B build \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON \
  -DFALCON_ENABLE_QQDL=OFF \
  -DFALCON_ENABLE_FLASHGET=OFF
```

## CLI Usage Examples

```bash
# Download a Thunder link
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg=="

# Download an ED2K link
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# Download HLS streaming content
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# Batch download (mixing protocols)
cat urls.txt
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS5yYXJaWg==
qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==
ed2k://file|test.zip|2048576|B2C3D4E5F6789012345678901234ABC|/
https://example.com/stream.m3u8

falcon-cli -i urls.txt
```

## API Programming Example

```cpp
#include <falcon/falcon.hpp>

int main() {
    // Create the download engine (construction auto-registers every built-in
    // protocol handler, including the private protocol plugins)
    falcon::DownloadEngine engine;

    // Check which protocols are supported
    auto protocols = engine.get_supported_protocols();
    std::cout << "Supported protocols: ";
    for (const auto& p : protocols) {
        std::cout << p << " ";
    }
    std::cout << std::endl;

    // Download links of different types
    falcon::DownloadOptions options;
    options.output_directory = "./downloads";

    std::vector<std::string> urls = {
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg==",
        "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/",
        "https://example.com/playlist.m3u8"
    };

    std::vector<falcon::DownloadTask::Ptr> tasks;

    for (const auto& url : urls) {
        if (engine.is_url_supported(url)) {
            auto task = engine.add_task(url, options);
            if (task) {
                engine.start_task(task->id());
                tasks.push_back(task);
                std::cout << "Starting download: " << url << std::endl;
            }
        } else {
            std::cout << "Unsupported URL: " << url << std::endl;
        }
    }

    // Wait for all downloads to finish
    for (auto& task : tasks) {
        task->wait();
    }

    return 0;
}
```

## Notes

1. **Security**: Private protocol links can contain arbitrary URLs; make sure the source is trustworthy.
2. **Legal compliance**: Use these protocols only to download lawful content.
3. **Network requirements**: Some protocols may require a specific network environment or proxy.
4. **Performance**: Download speed for P2P protocols such as ED2K depends on the number of sources available on the network.

## Testing

Run the unit tests for private protocols:

```bash
# Build the tests
cmake -B build -DFALCON_BUILD_TESTS=ON
cmake --build build

# Run tests for specific protocols (suite names: ThunderPluginTest /
# QQDLPluginTest / FlashGetPluginTest / ED2KPluginTest / HLSPluginTest)
ctest --test-dir build -R "ThunderPlugin"
ctest --test-dir build -R "ED2KPlugin"
ctest --test-dir build -R "HLSPlugin"

# Run all five private protocol suites at once
ctest --test-dir build -R "ThunderPlugin|QQDLPlugin|FlashGetPlugin|ED2KPlugin|HLSPlugin"
```

## Future Plans

- [ ] Support additional private protocols (e.g., cloud drive link parsing)
- [ ] Enhance ED2K network search and source discovery
- [ ] Support DRM-protected content in HLS
- [ ] Add automatic protocol detection
- [ ] Provide a protocol conversion utility
