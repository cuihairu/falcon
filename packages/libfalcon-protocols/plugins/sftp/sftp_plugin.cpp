/**
 * @file sftp_plugin.cpp
 * @brief SFTP 协议插件实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "sftp_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <fstream>
#include <sstream>
#include <regex>
#include <sys/stat.h>
#include <chrono>
#include <thread>

namespace falcon {
namespace plugins {

// ============================================================================
// SFTPPlugin 实现
// ============================================================================

SFTPPlugin::SFTPPlugin() {
    FALCON_LOG_INFO("SFTP plugin initialized");
}

SFTPPlugin::~SFTPPlugin() {
    FALCON_LOG_DEBUG("SFTP plugin shutdown");
}

std::vector<std::string> SFTPPlugin::getSupportedSchemes() const {
    return {"sftp"};
}

bool SFTPPlugin::canHandle(const std::string& url) const {
    return url.find("sftp://") == 0;
}

std::unique_ptr<IDownloadTask> SFTPPlugin::createTask(const std::string& url,
                                                       const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating SFTP task for: {}", url);
    return std::make_unique<SFTPDownloadTask>(url, options);
}

bool SFTPPlugin::parseSFTPUrl(const std::string& url, ConnectionInfo& info, std::string& path) {
    // SFTP URL 格式: sftp://[user[:password]@]host[:port]/path
    std::regex sftpRegex(
        R"(sftp://(?:([^:]+)(?::([^@]*))?@)?([^:/]+)(?::(\d+))?(/.*)?)"
    );

    std::smatch match;
    if (!std::regex_match(url, match, sftpRegex)) {
        FALCON_LOG_ERROR("Invalid SFTP URL format: {}", url);
        return false;
    }

    info.username = match[1].str();
    info.password = match[2].str();
    info.host = match[3].str();

    std::string portStr = match[4].str();
    info.port = portStr.empty() ? 22 : std::stoi(portStr);

    path = match[5].str();

    // 默认用户名
    if (info.username.empty()) {
        const char* user = std::getenv("USER");
        if (!user) user = std::getenv("USERNAME");
        if (user) info.username = user;
        else info.username = "root";
    }

    FALCON_LOG_DEBUG("Parsed SFTP URL: {}@{}:{}{}",
                     info.username, info.host, info.port, path);

    return true;
}

// ============================================================================
// SFTPDownloadTask 实现
// ============================================================================

SFTPPlugin::SFTPDownloadTask::SFTPDownloadTask(const std::string& url,
                                                const DownloadOptions& options)
    : url_(url)
    , options_(options)
    , status_(TaskStatus::Pending)
    , totalSize_(0)
    , downloadedBytes_(0)
    , downloadSpeed_(0) {
#ifdef FALCON_USE_LIBSSH
    sshSession_ = nullptr;
    sftpSession_ = nullptr;
    file_ = nullptr;
#endif
}

SFTPPlugin::SFTPDownloadTask::~SFTPDownloadTask() {
    if (status_ == TaskStatus::Downloading) {
        cancel();
    }
    disconnect();
}

bool SFTPPlugin::SFTPDownloadTask::parseUrl(const std::string& url) {
    SFTPPlugin plugin;
    return plugin.parseSFTPUrl(url, connInfo_, remotePath_);
}

bool SFTPPlugin::SFTPDownloadTask::loadConnectionInfo() {
    // 从环境变量读取 SSH 密钥路径
    const char* home = std::getenv("HOME");
    if (home) {
        std::string defaultKey = std::string(home) + "/.ssh/id_rsa";
        struct stat buffer;
        if (stat(defaultKey.c_str(), &buffer) == 0) {
            connInfo_.privateKeyPath = defaultKey;
            connInfo_.publicKeyPath = defaultKey + ".pub";
        }
    }

    // 可以从配置文件读取更多连接信息
    return true;
}

bool SFTPPlugin::SFTPDownloadTask::connect() {
#ifdef FALCON_USE_LIBSSH
    // 创建 SSH 会话
    sshSession_ = ssh_new();
    if (sshSession_ == nullptr) {
        errorMessage_ = "Failed to create SSH session";
        return false;
    }

    // 设置连接选项
    ssh_options_set(sshSession_, SSH_OPTIONS_HOST, connInfo_.host.c_str());
    ssh_options_set(sshSession_, SSH_OPTIONS_PORT, &connInfo_.port);
    ssh_options_set(sshSession_, SSH_OPTIONS_USER, connInfo_.username.c_str());

    // 设置超时
    long timeout = options_.timeout_seconds > 0 ? options_.timeout_seconds : 30;
    ssh_options_set(sshSession_, SSH_OPTIONS_TIMEOUT, &timeout);

    // 连接到服务器
    int rc = ssh_connect(sshSession_);
    if (rc != SSH_OK) {
        errorMessage_ = "Failed to connect: " + std::string(ssh_get_error(sshSession_));
        ssh_free(sshSession_);
        sshSession_ = nullptr;
        return false;
    }

    // 认证
    if (!authenticate()) {
        disconnect();
        return false;
    }

    // 创建 SFTP 会话
    sftpSession_ = sftp_new(sshSession_);
    if (sftpSession_ == nullptr) {
        errorMessage_ = "Failed to create SFTP session";
        disconnect();
        return false;
    }

    rc = sftp_init(sftpSession_);
    if (rc != SSH_OK) {
        errorMessage_ = "Failed to initialize SFTP session";
        disconnect();
        return false;
    }

    return true;
#else
    errorMessage_ = "SFTP support not compiled with libssh";
    return false;
#endif
}

void SFTPPlugin::SFTPDownloadTask::disconnect() {
#ifdef FALCON_USE_LIBSSH
    if (file_) {
        sftp_close(file_);
        file_ = nullptr;
    }

    if (sftpSession_) {
        sftp_free(sftpSession_);
        sftpSession_ = nullptr;
    }

    if (sshSession_) {
        ssh_disconnect(sshSession_);
        ssh_free(sshSession_);
        sshSession_ = nullptr;
    }
#endif
}

bool SFTPPlugin::SFTPDownloadTask::authenticate() {
#ifdef FALCON_USE_LIBSSH
    // 尝试密钥认证
    if (!connInfo_.privateKeyPath.empty()) {
        ssh_key privkey;
        int rc = ssh_pki_import_privkey_file(
            connInfo_.privateKeyPath.c_str(),
            connInfo_.passphrase.empty() ? nullptr : connInfo_.passphrase.c_str(),
            nullptr,
            nullptr,
            &privkey
        );

        if (rc == SSH_OK) {
            rc = ssh_userauth_publickey(sshSession_, nullptr, privkey);
            ssh_key_free(privkey);

            if (rc == SSH_AUTH_SUCCESS) {
                FALCON_LOG_INFO("Authenticated with public key");
                return true;
            }
        }
    }

    // 尝试密码认证
    if (!connInfo_.password.empty()) {
        int rc = ssh_userauth_password(sshSession_, nullptr, connInfo_.password.c_str());
        if (rc == SSH_AUTH_SUCCESS) {
            FALCON_LOG_INFO("Authenticated with password");
            return true;
        }
    }

    errorMessage_ = "Authentication failed";
    return false;
#else
    return false;
#endif
}

bool SFTPPlugin::SFTPDownloadTask::getRemoteFileSize() {
#ifdef FALCON_USE_LIBSSH
    sftp_attributes attr = sftp_stat(sftpSession_, remotePath_.c_str());
    if (attr == nullptr) {
        errorMessage_ = "Failed to get file attributes";
        return false;
    }

    if (attr->type == SSH_FILEXFER_TYPE_DIRECTORY) {
        errorMessage_ = "Remote path is a directory, not a file";
        sftp_attributes_free(attr);
        return false;
    }

    totalSize_ = attr->size;
    sftp_attributes_free(attr);

    FALCON_LOG_INFO("Remote file size: {} bytes", totalSize_);
    return true;
#else
    return false;
#endif
}

bool SFTPPlugin::SFTPDownloadTask::download() {
#ifdef FALCON_USE_LIBSSH
    // 打开远程文件
    file_ = sftp_open(sftpSession_, remotePath_.c_str(), O_RDONLY, 0);
    if (file_ == nullptr) {
        errorMessage_ = "Failed to open remote file";
        return false;
    }

    // 确定本地保存路径
    if (options_.output_path.empty()) {
        // 从 URL 中提取文件名
        size_t pos = remotePath_.find_last_of('/');
        localPath_ = pos != std::string::npos ? remotePath_.substr(pos + 1) : "download";
    } else {
        localPath_ = options_.output_path;
    }

    // 打开本地文件
    std::ofstream localFile(localPath_, std::ios::binary);
    if (!localFile.is_open()) {
        errorMessage_ = "Failed to create local file: " + localPath_;
        return false;
    }

    // 分块下载
    const size_t bufferSize = 32768;  // 32KB
    std::vector<char> buffer(bufferSize);

    auto startTime = std::chrono::steady_clock::now();
    auto lastUpdateTime = startTime;

    while (status_ == TaskStatus::Downloading) {
        ssize_t bytesRead = sftp_read(file_, buffer.data(), bufferSize);

        if (bytesRead < 0) {
            errorMessage_ = "Error reading from SFTP";
            status_ = TaskStatus::Failed;
            break;
        }

        if (bytesRead == 0) {
            // 下载完成
            status_ = TaskStatus::Completed;
            break;
        }

        // 写入本地文件
        localFile.write(buffer.data(), bytesRead);
        downloadedBytes_ += bytesRead;

        // 更新速度
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdateTime).count();

        if (elapsed >= 1) {
            downloadSpeed_ = downloadedBytes_ / elapsed;
            lastUpdateTime = now;

            FALCON_LOG_DEBUG("Downloaded: {}/{} ({:.1f}%)",
                           downloadedBytes_, totalSize_,
                           (totalSize_ > 0 ? (downloadedBytes_ * 100.0 / totalSize_) : 0));
        }

        // 检查限速
        if (options_.speed_limit > 0) {
            // 简单的限速实现
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    localFile.close();

    if (status_ == TaskStatus::Completed) {
        FALCON_LOG_INFO("Download completed: {}", localPath_);
    }

    return status_ == TaskStatus::Completed;
#else
    return false;
#endif
}

void SFTPPlugin::SFTPDownloadTask::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (status_ != TaskStatus::Pending) {
        return;
    }

    // 解析 URL
    if (!parseUrl(url_)) {
        errorMessage_ = "Failed to parse SFTP URL";
        status_ = TaskStatus::Failed;
        return;
    }

    // 加载连接信息
    if (!loadConnectionInfo()) {
        errorMessage_ = "Failed to load connection info";
        status_ = TaskStatus::Failed;
        return;
    }

    status_ = TaskStatus::Downloading;

    // 连接
    if (!connect()) {
        status_ = TaskStatus::Failed;
        return;
    }

    // 获取文件大小
    if (!getRemoteFileSize()) {
        status_ = TaskStatus::Failed;
        disconnect();
        return;
    }

    // 开始下载
    if (!download()) {
        if (errorMessage_.empty() && status_ != TaskStatus::Completed) {
            errorMessage_ = "Download failed";
        }
    }

    disconnect();
}

void SFTPPlugin::SFTPDownloadTask::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Downloading) {
        status_ = TaskStatus::Paused;
    }
}

void SFTPPlugin::SFTPDownloadTask::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Paused) {
        status_ = TaskStatus::Downloading;
    }
}

void SFTPPlugin::SFTPDownloadTask::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = TaskStatus::Cancelled;
    disconnect();
}

TaskStatus SFTPPlugin::SFTPDownloadTask::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

float SFTPPlugin::SFTPDownloadTask::getProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (totalSize_ == 0) return 0.0f;
    return static_cast<float>(downloadedBytes_) / totalSize_;
}

uint64_t SFTPPlugin::SFTPDownloadTask::getTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalSize_;
}

uint64_t SFTPPlugin::SFTPDownloadTask::getDownloadedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downloadedBytes_;
}

uint64_t SFTPPlugin::SFTPDownloadTask::getSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downloadSpeed_;
}

std::string SFTPPlugin::SFTPDownloadTask::getErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorMessage_;
}

} // namespace plugins
} // namespace falcon
