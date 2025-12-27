/**
 * @file sftp_plugin.hpp
 * @brief SFTP 协议插件
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

#ifdef FALCON_USE_LIBSSH
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#endif

namespace falcon {
namespace plugins {

/**
 * @class SFTPPlugin
 * @brief SFTP 协议处理器
 *
 * 支持 SFTP (SSH File Transfer Protocol) 下载
 * 基于 libssh 库实现
 */
class SFTPPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    SFTPPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~SFTPPlugin();

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "sftp"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief SFTP 连接信息
     */
    struct ConnectionInfo {
        std::string host;
        int port;
        std::string username;
        std::string password;
        std::string publicKeyPath;
        std::string privateKeyPath;
        std::string passphrase;
    };

    /**
     * @brief SFTP 下载任务
     */
    class SFTPDownloadTask : public IDownloadTask {
    public:
        SFTPDownloadTask(const std::string& url, const DownloadOptions& options);
        virtual ~SFTPDownloadTask();

        // IDownloadTask 接口实现
        void start() override;
        void pause() override;
        void resume() override;
        void cancel() override;
        TaskStatus getStatus() const override;
        float getProgress() const override;
        uint64_t getTotalBytes() const override;
        uint64_t getDownloadedBytes() const override;
        uint64_t getSpeed() const override;
        std::string getErrorMessage() const override;

    private:
#ifdef FALCON_USE_LIBSSH
        ssh_session sshSession_;
        sftp_session sftpSession_;
        sftp_file file_;
#endif

        std::string url_;
        DownloadOptions options_;
        ConnectionInfo connInfo_;
        std::string remotePath_;
        std::string localPath_;

        TaskStatus status_;
        std::string errorMessage_;

        // 统计信息
        uint64_t totalSize_;
        uint64_t downloadedBytes_;
        uint64_t downloadSpeed_;

        // 线程安全
        mutable std::mutex mutex_;

        /**
         * @brief 解析 SFTP URL
         */
        bool parseUrl(const std::string& url);

        /**
         * @brief 建立 SSH 连接
         */
        bool connect();

        /**
         * @brief 断开连接
         */
        void disconnect();

        /**
         * @brief 认证（密码或密钥）
         */
        bool authenticate();

        /**
         * @brief 获取远程文件大小
         */
        bool getRemoteFileSize();

        /**
         * @brief 下载文件
         */
        bool download();

        /**
         * @brief 从环境变量或配置文件读取连接信息
         */
        bool loadConnectionInfo();
    };

    /**
     * @brief 解析 SFTP URL
     * 格式: sftp://[user@]host[:port]/path
     *       sftp://user:pass@host:port/path
     */
    bool parseSFTPUrl(const std::string& url, ConnectionInfo& info, std::string& path);
};

} // namespace plugins
} // namespace falcon
