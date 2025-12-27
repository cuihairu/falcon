#pragma once

#include <falcon/download_engine.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace falcon::daemon::rpc {

struct JsonRpcServerConfig {
    uint16_t listen_port = 6800;
    std::string secret;
    bool allow_origin_all = false;
    std::string bind_address = "127.0.0.1";
};

class JsonRpcServer {
public:
    struct HttpRequest;
    struct HttpResponse;

    explicit JsonRpcServer(falcon::DownloadEngine* engine, JsonRpcServerConfig config);
    ~JsonRpcServer();

    JsonRpcServer(const JsonRpcServer&) = delete;
    JsonRpcServer& operator=(const JsonRpcServer&) = delete;

    bool start();
    void stop();

    uint16_t port() const noexcept { return config_.listen_port; }

private:
    void accept_loop();
    void handle_connection(int client_fd);

    HttpResponse handle_http_request(const HttpRequest& req);
    HttpResponse handle_jsonrpc(const std::string& body);

    falcon::DownloadEngine* engine_ = nullptr;
    JsonRpcServerConfig config_;

    std::atomic<bool> stop_requested_{false};
    std::thread accept_thread_;
    std::mutex worker_threads_mutex_;
    std::vector<std::thread> worker_threads_;

    int listen_fd_ = -1;
};

} // namespace falcon::daemon::rpc
