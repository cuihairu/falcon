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
// Create the Thunder plugin and register it
auto thunderPlugin = std::make_unique<ThunderPlugin>();
engine.registerPlugin(std::move(thunderPlugin));

// Download a Thunder link
std::string url = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==";
auto task = engine.startDownload(url, options);
```

### 2. QQDL Protocol

**Protocol identifiers**: `qqlink://` and `qqdl://`

**Description**: QQDL is a download tool from Tencent that supports sharing resources through special links.

**Link format**:
- GID format: `qqlink://[GID]|[file info]`
- Base64 format: `qqlink://[Base64-encoded URL]`

**Implementation details**:
- GID (group ID) resolution
- Base64 decoding
- File info extraction (filename, size, CID, etc.)

**Example**:
```cpp
auto qqPlugin = std::make_unique<QQDLPlugin>();
engine.registerPlugin(std::move(qqPlugin));

std::string url = "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==";
auto task = engine.startDownload(url, options);
```

### 3. FlashGet Protocol

**Protocol identifiers**: `flashget://` and `fg://`

**Description**: FlashGet is a veteran download manager.

**Link format**:
- Full format: `flashget://[URL]&ref=[referring page]`
- Short format: `fg://[URL]`
- Base64 format: `flashget://[Base64-encoded URL]`

**Implementation details**:
- URL decoding
- Referring page extraction
- Automatic conversion to a standard HTTP download

**Example**:
```cpp
auto flashgetPlugin = std::make_unique<FlashGetPlugin>();
engine.registerPlugin(std::move(flashgetPlugin));

std::string url = "flashget://W10=";  // example link
auto task = engine.startDownload(url, options);
```

### 4. ED2K Protocol (eDonkey2000)

**Protocol identifier**: `ed2k://`

**Description**: ED2K is the protocol used by the eDonkey2000 network, also known as the eDonkey protocol, for P2P file sharing.

**Link format**:
- File link: `ed2k://file|[filename]|[file size]|[MD4 hash]|/`
- Server link: `ed2k://server|[server address]|[port]|[server name]/`

**Implementation details**:
- MD4 hash verification
- File size parsing
- Source address extraction
- Support for AICH (Advanced Intelligent Corruption Handling) hashes

**Example**:
```cpp
auto ed2kPlugin = std::make_unique<ED2KPlugin>();
engine.registerPlugin(std::move(ed2kPlugin));

std::string url = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";
auto task = engine.startDownload(url, options);
```

### 5. HLS/DASH Streaming Protocols

**Protocol identification**: by file extension (`.m3u8`, `.mpd`)

**Description**: HTTP Live Streaming (HLS) and Dynamic Adaptive Streaming over HTTP (DASH) are commonly used streaming protocols.

**HLS format**: `.m3u8` playlist file
**DASH format**: `.mpd` media presentation description file

**Implementation details**:
- Playlist/manifest parsing
- Media segment download
- Automatic quality selection
- Media segment merging using ffmpeg

**Example**:
```cpp
auto hlsPlugin = std::make_unique<HLSPlugin>();
engine.registerPlugin(std::move(hlsPlugin));

// Download an HLS stream
std::string hlsUrl = "https://example.com/playlist.m3u8";
auto task1 = engine.startDownload(hlsUrl, options);

// Download a DASH stream
std::string dashUrl = "https://example.com/manifest.mpd";
auto task2 = engine.startDownload(dashUrl, options);
```

## Build Configuration

Use CMake options to enable or disable specific protocols:

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
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# Download an ED2K link
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# Download HLS streaming content
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# Batch download (mixing protocols)
cat urls.txt
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsGUucmFyWg==
qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==
ed2k://file|test.zip|2048576|B2C3D4E5F6789012345678901234ABC|/
https://example.com/stream.m3u8

falcon-cli -i urls.txt
```

## API Programming Example

```cpp
#include <falcon/falcon.hpp>
#include <falcon/thunder_plugin.hpp>
#include <falcon/ed2k_plugin.hpp>
#include <falcon/hls_plugin.hpp>

int main() {
    // Create the download engine
    falcon::DownloadEngine engine;

    // Register private protocol plugins
    engine.registerPlugin(std::make_unique<falcon::ThunderPlugin>());
    engine.registerPlugin(std::make_unique<falcon::ED2KPlugin>());
    engine.registerPlugin(std::make_unique<falcon::HLSPlugin>());

    // Check which protocols are supported
    auto protocols = engine.listSupportedProtocols();
    std::cout << "Supported protocols: ";
    for (const auto& p : protocols) {
        std::cout << p << " ";
    }
    std::cout << std::endl;

    // Download links of different types
    falcon::DownloadOptions options;
    options.output_directory = "./downloads";

    std::vector<std::string> urls = {
        "thunder://...",
        "ed2k://...",
        "https://example.com/playlist.m3u8"
    };

    std::vector<std::shared_ptr<falcon::DownloadTask>> tasks;

    for (const auto& url : urls) {
        if (engine.supportsUrl(url)) {
            auto task = engine.startDownload(url, options);
            tasks.push_back(task);
            std::cout << "Starting download: " << url << std::endl;
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

# Run tests for specific protocols
ctest --test-dir build -R "thunder_plugin_test"
ctest --test-dir build -R "ed2k_plugin_test"
ctest --test-dir build -R "hls_plugin_test"

# Run all private protocol tests
ctest --test-dir build -R "private_protocol"
```

## Future Plans

- [ ] Support additional private protocols (e.g., cloud drive link parsing)
- [ ] Enhance ED2K network search and source discovery
- [ ] Support DRM-protected content in HLS
- [ ] Add automatic protocol detection
- [ ] Provide a protocol conversion utility
