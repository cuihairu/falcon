# Release Process

This document explains how to publish a new Falcon release.

## Version numbering

Falcon follows [Semantic Versioning](https://semver.org/):

```
MAJOR.MINOR.PATCH

1.0.0 - Initial stable release
1.1.0 - New features (backward compatible)
1.1.1 - Bug fixes
2.0.0 - Breaking changes
```

### Release types

| Type | Description | Example |
|------|------|------|
| **Major** | Breaking API changes | 1.0.0 → 2.0.0 |
| **Minor** | Backward-compatible features | 1.0.0 → 1.1.0 |
| **Patch** | Backward-compatible bug fixes | 1.0.0 → 1.0.1 |

## Pre-release checklist

### Code quality

- [ ] All tests pass
- [ ] Code coverage meets the target
- [ ] No compiler warnings
- [ ] Code-format checks pass
- [ ] Static analysis passes

### Documentation

- [ ] CHANGELOG updated
- [ ] README updated when needed
- [ ] API documentation updated
- [ ] Migration guide available (major releases)

### Features

- [ ] All planned features implemented
- [ ] Known bugs fixed
- [ ] Performance tests pass

## Release steps

### 1. Update the version number

```cpp
// include/falcon/version.hpp
#define FALCON_VERSION_MAJOR 1
#define FALCON_VERSION_MINOR 1
#define FALCON_VERSION_PATCH 0
#define FALCON_VERSION "1.1.0"
```

### 2. Update CHANGELOG

```markdown
## [1.1.0] - 2025-01-06

### Added
- Added DASH protocol support
- Added download speed limiting
- Added configuration file support

### Changed
- Improved HTTP connection reuse
- Raised the minimum C++ standard to C++17

### Fixed
- Fixed file-size calculation for resumed downloads
- Fixed FTP active-mode connection failures

### Security
- Fixed a potential SSL certificate validation issue
```

### 3. Create a release branch

```bash
# Create a release branch from main
git checkout -b release/1.1.0

# Update the version and CHANGELOG
git add .
git commit -m "chore: prepare 1.1.0 release"
```

### 4. Test the release build

```bash
# Build the release configuration
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release

# Run tests
ctest --test-dir build-release --output-on-failure

# Smoke-test the CLI
./build-release/bin/falcon-cli --version
./build-release/bin/falcon-cli https://example.com/test.zip
```

### 5. Create the Git tag

```bash
# Create an annotated tag
git tag -a v1.1.0 -m "Release version 1.1.0"

# Push the tag
git push origin v1.1.0
```

### 6. Merge into main

```bash
# Merge the release branch into main
git checkout main
git merge release/1.1.0

# Push
git push origin main
```

### 7. Create a GitHub Release

1. Open [GitHub Releases](https://github.com/cuihairu/falcon/releases).
2. Click "Draft a new release".
3. Select the `v1.1.0` tag.
4. Enter the release title: `Falcon 1.1.0`.
5. Paste the release notes from CHANGELOG.
6. Upload the built release packages.
7. Click "Publish release".

## Build release packages

### Linux

```bash
# Use CPack
cmake --build build-release --target package

# Or package manually
cd build-release
make package
```

### macOS

```bash
# Create a DMG
cmake --build build-release --target package

# Or use MacPorts
sudo port install falcon
```

### Windows

```bash
# Use CPack
cmake --build build-release --config Release --target package

# Or use WiX
candle falcon.wxs
light -out falcon.msi falcon.wixobj
```

## Continuous integration

### GitHub Actions

```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  release:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        include:
          - os: ubuntu-latest
            artifact_name: falcon-linux-x64.tar.gz
          - os: macos-latest
            artifact_name: falcon-macos-x64.tar.gz
          - os: windows-latest
            artifact_name: falcon-windows-x64.zip

    steps:
    - uses: actions/checkout@v3

    - name: Install dependencies
      run: |
        ./vcpkg install spdlog nlohmann-json curl openssl

    - name: Configure CMake
      run: |
        cmake -B build -S . \
          -DCMAKE_BUILD_TYPE=Release \
          -DFALCON_BUILD_TESTS=ON \
          -DCMAKE_TOOLCHAIN_FILE=${{ github.workspace }}/vcpkg/scripts/buildsystems/vcpkg.cmake

    - name: Build
      run: cmake --build build --config Release

    - name: Run tests
      run: ctest --test-dir build --output-on-failure

    - name: Package
      run: cmake --build build --target package

    - name: Upload artifacts
      uses: actions/upload-artifact@v3
      with:
        name: ${{ matrix.artifact_name }}
        path: build/falcon-*
```

## After release

### Notify users

- [ ] Publish the GitHub Release
- [ ] Update the website home page
- [ ] Send an email announcement if a mailing list exists
- [ ] Announce on social media

### Update documentation

- [ ] Update the website documentation
- [ ] Add a migration guide when needed
- [ ] Update API documentation

### Monitor

- [ ] Monitor issues and pull requests
- [ ] Collect user feedback
- [ ] Track download statistics

## Rollback plan

If a serious issue is discovered:

```bash
# Remove the tag locally and remotely
git tag -d v1.1.0
git push origin :refs/tags/v1.1.0

# Delete the GitHub Release in the web UI.

# Publish a fixed release, for example 1.1.1.
```

::: tip Release guidance
- Release outside working hours so there is time to handle urgent issues.
- Tell users in advance about an upcoming release.
- Have a fast-fix plan ready.
:::
