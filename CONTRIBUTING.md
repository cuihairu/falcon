# Contributing to Falcon

Thanks for your interest in contributing! This document covers the basics.

## Getting Started

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon

# Configure with tests enabled
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

See `README.md` (Prerequisites) for required toolchains and libraries.

## Project Layout

Falcon is a CMake monorepo:

- `packages/libfalcon-core` — download engine, task manager, event system
- `packages/libfalcon-protocols` — HTTP/FTP/BT/Metalink/HLS protocol implementations
- `packages/libfalcon-storage` — S3/OSS/COS/Kodo/Upyun object storage browsing
- `packages/libfalcon-drives` — cloud drives, search, encrypted config manager
- `packages/falcon-cli` — command-line downloader
- `packages/falcon-daemon` — RPC service with aria2-compatible JSON-RPC API
- `apps/desktop` — Qt6 desktop application

Dependency direction: `protocols/storage/drives → core`. Reverse dependencies are not allowed.

## Code Style

- Follow the Google C++ Style Guide; the project uses `snake_case` for functions/variables and `PascalCase` for types
- Format with `clang-format` (config in `.clang-format`)
- C++17; keep code warning-clean across GCC / Clang / MSVC

## Tests

- Add unit tests for new features under the owning package's `tests/` directory (Google Test)
- Network-facing code should be tested against local scripted/mock servers, not live services
- Run the full suite before submitting: `ctest --test-dir build --output-on-failure`

## Pull Requests

1. Create a feature branch; keep changes focused
2. Ensure the build and tests pass on your platform
3. Describe what changed and why in the PR description

## License

By contributing, you agree that your contributions will be licensed under the Apache License 2.0.
