#pragma once

// ============================================================================
// SwarmWsSubscriber：swarm 目录服务器的收订专用 WebSocket 客户端
//
// 与 daemon 的 WebSocketRpcClient 形态不同：只收不发业务请求（无 pending
// call 机器）——握手带 Authorization: Bearer（服务器 /jsonrpc 升级门），
// WsFrameParser 收帧，ping→pong 应答，断线后退避重连，text 帧回调上抛。
//
// 线程模型：start() 起唯一工作线程（连接→收帧→失败退避→重连循环）；
// stop() 先 shutdown 当前 fd 唤醒阻塞 recv 再 join（daemon 停机先例——
// Linux close() 不唤醒阻塞 recv）。回调在工作线程内同步执行，消费方
// 自行保证回调体轻量或内部转投队列。
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace falcon::swarm {

class SwarmWsSubscriber {
public:
    struct Options {
        std::string host = "127.0.0.1";
        int port = 0;
        std::string bearer_token;  // 非空才带 Authorization 头
        std::chrono::milliseconds reconnect_delay{2000};
        std::chrono::milliseconds connect_timeout{3000};
    };

    using TextHandler = std::function<void(const std::string&)>;

    explicit SwarmWsSubscriber(Options opt);
    ~SwarmWsSubscriber();

    SwarmWsSubscriber(const SwarmWsSubscriber&) = delete;
    SwarmWsSubscriber& operator=(const SwarmWsSubscriber&) = delete;

    void set_text_handler(TextHandler handler);

    /// 启动工作线程（立即返回，连接在后台进行；观察连接用计数器或
    /// 首条通知到达判定）。已在运行则无操作返回 true。
    bool start();

    /// RAII 收口：shutdown fd 唤醒 recv → join 工作线程。可重复调用。
    void stop();

    bool is_running() const;
    uint64_t connect_attempts() const { return connect_attempts_.load(); }
    uint64_t connected_count() const { return connected_count_.load(); }

private:
    void run_loop();
    /// 建立 TCP + 完成 WS 握手；成功返回 fd（所有权归 run_loop）。
    /// 失败返回负值。connect_attempts_/connected_count_ 在此记账。
    int connect_once(std::string* error);
    /// 帧发送（fd 写互斥；fd 已失效返回 false）。
    bool send_frame(int fd, uint8_t opcode, const std::string& payload);
    /// 从 fd 收帧直到断连/CLOSE；text 帧走 handler。返回是否正常收尾
    /// （CLOSE 帧回执后干净断开 vs 传输错误——两者都触发重连，仅日志
    /// 区分）。
    void recv_loop(int fd);

    Options opt_;
    TextHandler text_handler_;  // start 前设置；run_loop 只读
    std::mutex fd_mutex_;       // 保护 fd_ 的关闭与并发写
    int fd_ = -1;               // 当前连接（-1 = 无）
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> connect_attempts_{0};
    std::atomic<uint64_t> connected_count_{0};
};

}  // namespace falcon::swarm
