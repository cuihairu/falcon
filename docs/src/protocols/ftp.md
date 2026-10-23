# FTP/FTPS Protocol

Falcon supports FTP and FTPS and includes an FTP handler by default.

## Features

- FTP downloads
- FTPS links
- Username/password authentication
- Anonymous access
- Resume
- Proxy passthrough

## Command-line usage

### Basic download

```bash
falcon-cli ftp://ftp.example.com/file.zip
falcon-cli ftp://user:password@ftp.example.com/file.zip
falcon-cli ftp://ftp.example.com:2121/file.zip
```

### FTPS

```bash
falcon-cli ftpes://ftp.example.com/file.zip
falcon-cli ftps://ftp.example.com/file.zip
falcon-cli ftpes://user:password@ftp.example.com/file.zip
```

### Resume and proxy

```bash
falcon-cli ftp://ftp.example.com/large.zip --continue true
falcon-cli ftp://ftp.example.com/file.zip --proxy socks5://proxy:1080
```

## C++ API

The current public API does not expose a separate `FtpOptions` struct; the usual approach is a URL plus `DownloadOptions`.

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.output_directory = "./downloads";
    options.output_filename = "file.zip";
    options.resume_enabled = true;
    options.timeout_seconds = 30;

    auto task = engine.add_task(
        "ftp://user:password@ftp.example.com/file.zip",
        options
    );

    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## URL formats

```text
ftp://[user[:password]@]host[:port]/path
ftpes://[user[:password]@]host[:port]/path
ftps://[user[:password]@]host[:port]/path
```

## Notes

- Usernames and passwords are usually supplied directly in the URL.
- `DownloadOptions` handles connection, retry, output-directory, and speed-limit behavior.
- If finer FTP-specific behavior enters the public headers in the future, document it separately here.

::: warning Security
Plain FTP transmits usernames and passwords in clear text. Prefer FTPS whenever it is available.
:::
