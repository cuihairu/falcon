# HTTP/HTTPS Protocol

HTTP/HTTPS is Falcon's most stable download protocol and is enabled by default.

## Features

- Resume
- Multi-connection segmented downloads
- Custom request headers
- Proxy support
- HTTP authentication
- SSL/TLS verification toggle

## Command-line usage

### Basic download

```bash
falcon-cli http://example.com/file.zip
falcon-cli https://example.com/file.zip
```

### Multi-connection download

```bash
falcon-cli https://example.com/large.zip -c 5
falcon-cli https://example.com/very-large.zip -x 16 -s 16
```

### Resume

```bash
falcon-cli https://example.com/file.zip --continue true
```

### Custom headers and authentication

```bash
falcon-cli https://example.com/file.zip \
  --user-agent "MyApp/2.0" \
  --header "Authorization: Bearer TOKEN" \
  --http-user user \
  --http-passwd pass
```

### Proxy

```bash
falcon-cli https://example.com/file.zip \
  --proxy http://proxy.example.com:8080 \
  --proxy-user user \
  --proxy-passwd pass
```

## C++ API

The current public API configures HTTP downloads through `DownloadOptions`; it does not expose a separate `HttpOptions` struct.

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.output_filename = "file.zip";
    options.user_agent = "MyApp/2.0";
    options.headers["Authorization"] = "Bearer TOKEN";
    options.proxy = "http://proxy.example.com:8080";
    options.proxy_username = "user";
    options.proxy_password = "pass";
    options.verify_ssl = true;
    options.resume_enabled = true;

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## Event listening

```cpp
class ProgressListener : public falcon::IEventListener {
public:
    void on_progress(const falcon::ProgressInfo& info) override {
        std::cout << "\r"
                  << std::fixed << std::setprecision(1)
                  << info.progress * 100.0f << "%";
        std::cout.flush();
    }

    void on_completed(falcon::TaskId, const std::string& output_path) override {
        std::cout << "\ncompleted: " << output_path << std::endl;
    }
};
```

## Relevant configuration fields

These `DownloadOptions` fields are commonly used for HTTP/HTTPS downloads:

| Field | Default | Description |
|------|--------|------|
| `max_connections` | `4` | Maximum connection count |
| `timeout_seconds` | `30` | Request timeout |
| `max_retries` | `3` | Maximum retry count |
| `retry_delay_seconds` | `1` | Retry interval |
| `speed_limit` | `0` | Per-task speed limit |
| `resume_enabled` | `true` | Enable resume |
| `user_agent` | `Falcon/0.1.0` | User-Agent |
| `headers` | `{}` | Custom headers |
| `proxy` | `""` | Proxy URL |
| `http_username` | `""` | HTTP username |
| `http_password` | `""` | HTTP password |
| `verify_ssl` | `true` | Verify certificates |

## FAQ

### Q: Why are multi-connection downloads slower?

A: The server may limit concurrency per IP, or the file may be too small. Try lowering `-c` / `-x`.

### Q: SSL certificate verification fails?

A: For temporary testing only, verification can be disabled:

```cpp
falcon::DownloadOptions options;
options.verify_ssl = false;
```

::: tip Recommendation
Keep `verify_ssl = true` in production.
:::
