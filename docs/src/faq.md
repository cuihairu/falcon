# Frequently Asked Questions

This page collects frequently asked questions about Falcon.

## Installation

### Q: How do I install Falcon?

A: Building from source is currently the recommended approach:

```bash
# Build from source
git clone https://github.com/cuihairu/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

See the [Installation Guide](./guide/installation.md) for details.

### Q: The build cannot find dependencies?

A: Make sure all dependencies are installed:

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest

# Ubuntu/Debian
sudo apt install libspdlog-dev nlohmann-json3-dev \
  libcurl4-openssl-dev libssl-dev libgtest-dev
```

### Q: How can I see which protocols are supported?

A: For now, check the [Protocol Support](/protocols/) documentation, or verify that the corresponding `FALCON_ENABLE_*` CMake options are enabled at build time.

## Usage

### Q: How do I download a file?

A: Basic usage:

```bash
falcon-cli https://example.com/file.zip
```

### Q: How do I download with multiple threads?

A: Use the `-c` option:

```bash
falcon-cli https://example.com/large.zip -c 5
```

### Q: Resuming doesn't work?

A: Make sure:
1. The server supports Range requests
2. You are using the `--continue` option
3. The file has not changed

### Q: How do I limit the download speed?

A: Use the `--limit` option:

```bash
falcon-cli https://example.com/file.zip --limit 1M
```

### Q: How do I use a proxy?

A: Use the `--proxy` option:

```bash
falcon-cli https://example.com/file.zip --proxy http://proxy:8080
```

## Protocols

### Q: Which protocols are supported?

A: Falcon supports:
- HTTP/HTTPS
- FTP/FTPS
- BitTorrent (Magnet, .torrent)
- ED2K
- Thunder (thunder://)
- QQ Whirlwind (qqlink://)
- FlashGet (flashget://)
- HLS/DASH

See [Protocol Support](/protocols/) for details.

### Q: How do I download a Thunder link?

A: Just use the Thunder link directly:

```bash
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
```

### Q: Magnet downloads are slow?

A: Possible causes:
1. No peers: add more trackers
2. Firewall blocking: open port 6881
3. Enable UPnP or set up a manual port mapping

### Q: HLS download fails?

A: Make sure:
1. The HLS/DASH plugin was enabled at build time (`FALCON_ENABLE_HLS=ON`)
2. The playlist URL is valid

## Configuration

### Q: Where is the configuration file?

A: Configuration file locations:

- **Linux/macOS**: `~/.config/falcon/config.json`
- **Windows**: `%APPDATA%\falcon\config.json`

### Q: How do I change the default download directory?

A: Edit the configuration file:

```json
{
  "default_download_dir": "~/Downloads"
}
```

### Q: How do I set a global proxy?

A: Edit the configuration file:

```json
{
  "proxy": "http://proxy:8080"
}
```

## Troubleshooting

### Q: What should I do if a download fails?

A: Check:
1. Whether your network connection is working
2. Whether the URL is valid
3. Use `--verbose` for detailed logs

### Q: SSL certificate verification fails?

A: Workaround (not recommended for production):

```json
{
  "verify_ssl": false
}
```

### Q: Not enough disk space?

A: Check disk space:

```bash
df -h
```

Or change the download directory:

```bash
falcon-cli <URL> -d /path/to/larger/disk
```

### Q: Permission denied?

A: Check file permissions:

```bash
# Check permissions on the target directory
ls -la ~/Downloads

# Fix permissions
chmod u+w ~/Downloads
```

## Performance

### Q: How do I improve download speed?

A: Try:
1. Increase the number of connections: `-c 5`
2. Use a faster network
3. Switch to a different CDN or mirror
4. Close other programs consuming bandwidth

### Q: Falcon uses too much memory?

A: Check:
1. The number of simultaneous download tasks
2. The maximum number of connections per task
3. Whether there is a memory leak

## Development

### Q: How do I contribute code?

A: See the [Developer Guide](/developer/).

### Q: How do I develop a new plugin?

A: See the [Plugin Development Guide](./developer/plugins.md).

### Q: How do I run the tests?

A:

```bash
# Build the tests
cmake -B build -S . -DFALCON_BUILD_TESTS=ON
cmake --build build

# Run the tests
ctest --test-dir build --output-on-failure
```

## Miscellaneous

### Q: Is Falcon open source?

A: Yes, it is open source under the Apache 2.0 license.

### Q: How does Falcon compare to aria2?

A: See the [Developer Guide](/developer/).

### Q: How do I get help?

A:
- Read the [documentation](./)
- Ask in [GitHub Issues](https://github.com/cuihairu/falcon/issues)
- Join [Discussions](https://github.com/cuihairu/falcon/discussions)

::: tip Still stuck?
Please ask in [GitHub Issues](https://github.com/cuihairu/falcon/issues) — we are happy to help!
:::
