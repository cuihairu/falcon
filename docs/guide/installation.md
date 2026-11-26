# Installation Guide

This document explains in detail how to build and install Falcon in the current state of the repository.

## Table of Contents

- [System Requirements](#system-requirements)
- [Building from Source](#building-from-source)
- [Verifying the Installation](#verifying-the-installation)

## System Requirements

### Minimum Requirements

| Component | Requirement |
|-----------|-------------|
| Operating system | Windows 10+, macOS 10.14+, Linux (major distributions) |
| CPU | x86_64 or ARM64 |
| Memory | 512 MB |
| Disk space | 100 MB |

### Build Requirements

| Component | Version |
|-----------|---------|
| CMake | 3.15+ |
| C++ compiler | GCC 7+, Clang 5+, MSVC 2017+ |
| Python | Not required |

The documentation in this repository assumes building from source. If Release artifacts or system packages are provided in the future, follow the repository release page and the CI artifact notes rather than the static placeholder examples in this document.

## Building from Source

### Dependencies

#### Core Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| spdlog | 1.9+ | Logging |
| nlohmann/json | 3.10+ | JSON parsing |
| libcurl | 7.68+ | HTTP client |
| OpenSSL | 1.1+ | SSL/TLS support |

#### Optional Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| libtorrent | 2.0+ | BitTorrent support |
| gtest | 1.12+ | Unit testing |

### Using vcpkg (Recommended)

#### 1. Install vcpkg

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
```

#### 2. Install Dependencies

```bash
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

#### 3. Build

```bash
# Get the source
git clone https://github.com/cuihairu/falcon.git
cd falcon

# Configure CMake
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release
```

### Using System Package Managers

#### macOS

```bash
brew install spdlog nlohmann-json curl openssl googletest cmake

cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)

cmake --build build --config Release
```

#### Ubuntu/Debian

```bash
sudo apt install \
  build-essential \
  cmake \
  git \
  libspdlog-dev \
  nlohmann-json3-dev \
  libcurl4-openssl-dev \
  libssl-dev \
  libgtest-dev

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

#### Windows (Visual Studio)

```powershell
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

.\vcpkg install spdlog nlohmann-json curl openssl gtest

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

### Common Build Options

The root `CMakeLists.txt` currently exposes these common options:

| Option | Default | Description |
|--------|---------|-------------|
| `FALCON_BUILD_TESTS` | `ON` | Build tests |
| `FALCON_BUILD_EXAMPLES` | `ON` | Build examples |
| `FALCON_BUILD_CLI` | `ON` | Build the CLI |
| `FALCON_BUILD_DAEMON` | `ON` | Build the daemon |
| `FALCON_BUILD_DESKTOP` | `ON` | Build the desktop app |
| `FALCON_ENABLE_HTTP` | `ON` | Enable the HTTP/HTTPS plugin |
| `FALCON_ENABLE_FTP` | `ON` | Enable the FTP plugin |
| `FALCON_ENABLE_BITTORRENT` | `ON` | Enable the BitTorrent plugin |
| `FALCON_ENABLE_THUNDER` | `OFF` | Enable the Thunder plugin |
| `FALCON_ENABLE_QQDL` | `OFF` | Enable the QQ Whirlwind plugin |
| `FALCON_ENABLE_FLASHGET` | `OFF` | Enable the FlashGet plugin |
| `FALCON_ENABLE_ED2K` | `OFF` | Enable the ED2K plugin |
| `FALCON_ENABLE_HLS` | `OFF` | Enable the HLS/DASH plugin |

Example:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DFALCON_BUILD_DESKTOP=OFF \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_HLS=ON
```

## Verifying the Installation

```bash
./build/bin/falcon-cli --version
```

If you built only a subset of the targets, run the corresponding binaries from their actual output directories.
