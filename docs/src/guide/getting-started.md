[中文](getting-started.zh.md) | **English**

# Getting Started

This guide follows the current state of the repository and walks you through building and running the Falcon CLI from source.

## Prerequisites

- **Operating system**: Windows 10+, macOS 10.14+, or Linux (major distributions)
- **Compilers**:
  - GCC 7+ (Linux)
  - Clang 5+ (macOS)
  - MSVC 2017+ (Windows)
- **CMake**: 3.15 or newer

## Installation

### Build from Source (Currently Recommended)

#### 1. Get the Source

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
```

#### 2. Install Dependencies

**Using vcpkg (recommended)**:

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

**Or use your system package manager**:

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest

# Ubuntu/Debian
sudo apt install libspdlog-dev nlohmann-json3-dev libcurl4-openssl-dev libssl-dev libgtest-dev
```

#### 3. Build

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

This documentation only guarantees the build-from-source path. If prebuilt releases or system packages become available later, refer to the repository release page and the CI artifact notes instead.

## Verify the Installation

```bash
falcon-cli --version
```

You should see output similar to:

```
Falcon CLI v0.2.0
```

## Your First Download

### Downloading from the Command Line

```bash
# Download a single file
falcon-cli https://example.com/file.zip

# Specify the output file name
falcon-cli https://example.com/file.zip -o my-file.zip

# Specify the save directory
falcon-cli https://example.com/file.zip -d ~/Downloads

# Multi-threaded download
falcon-cli https://example.com/large.zip -c 5

# Show download progress and detailed logs
falcon-cli https://example.com/file.zip --verbose
```

### Downloading Different Link Types

#### HTTP/HTTPS

```bash
falcon-cli https://example.com/file.zip
```

#### Thunder Links

```bash
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
```

#### Magnet Links

```bash
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example"
```

#### ED2K Links

```bash
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"
```

#### HLS Streaming

```bash
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4
```

## CLI Options Available Today

The CLI arguments in the repository are defined in `packages/falcon-cli/src/main.cpp`; the table below lists the commonly used options that are already implemented in the code.

| Option | Description | Example |
|--------|-------------|---------|
| `-o`, `--output` | Specify the output file name | `-o file.zip` |
| `-d`, `--directory` | Specify the output directory | `-d ~/Downloads` |
| `-c`, `--connections` | Set the number of concurrent connections | `-c 5` |
| `-j`, `--max-concurrent-downloads` | Set the maximum number of concurrent tasks | `-j 3` |
| `--limit` | Per-task speed limit | `--limit 1M` |
| `--retry-wait` | Set the retry wait in seconds | `--retry-wait 5` |
| `--proxy` | Specify a proxy address | `--proxy http://127.0.0.1:7890` |
| `-H`, `--header` | Add an HTTP header | `-H "Authorization: Bearer token"` |

## Batch Downloads

Create a text file `urls.txt`:

```
# Comment lines start with #
https://example.com/file1.zip
https://example.com/file2.zip
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTMuemlwWg==
```

Then download in batch:

```bash
falcon-cli -i urls.txt -j 3
```

## Next Steps

- [Installation Guide](installation.md) - Detailed build and dependency notes
- [Usage Guide](usage.md) - A deeper look at CLI options and the C++ API
- [Protocol Support](/protocols/) - Learn about all supported download protocols

## Getting Help

```bash
# Show help
falcon-cli --help

# Show the version
falcon-cli --version
```

::: tip Tip
Use the `--verbose` option to see detailed download logs, which helps with troubleshooting.
:::
