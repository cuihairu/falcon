/**
 * @file cloud_storage_plugin.hpp
 * @brief 网盘下载插件接口定义
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <regex>

namespace falcon {

/**
 * @brief 网盘文件信息
 */
struct CloudFileInfo {
    std::string id;               // 文件ID
    std::string name;             // 文件名
    size_t size = 0;              // 文件大小
    std::string type;             // 文件类型 (file/folder)
    std::string md5;              // MD5哈希
    std::string modified_time;    // 修改时间
    std::string download_url;     // 下载链接
    std::string share_url;        // 分享链接
    std::string password;         // 提取密码
    std::map<std::string, std::string> metadata; // 额外元数据
};

/**
 * @brief 网盘平台类型
 */
enum class CloudPlatform {
    Unknown,
    BaiduNetdisk,    // 百度网盘
    LanzouCloud,     // 蓝奏云
    AlibabaCloud,    // 阿里云盘
    TencentWeiyun,   // 腾讯微云
    115Cloud,        // 115网盘
    Quark,           // 夸克网盘
    PikPak,          // PikPak
    Mega,            // MEGA
    GoogleDrive,     // Google Drive
    OneDrive,        // OneDrive
    Dropbox,         // Dropbox
    YandexDisk       // Yandex Disk
};

/**
 * @brief 网盘提取结果
 */
struct CloudExtractionResult {
    bool success = false;
    std::string error_message;
    std::vector<CloudFileInfo> files;
    std::string platform_name;
    CloudPlatform platform_type = CloudPlatform::Unknown;
};

/**
 * @brief 网盘下载选项
 */
struct CloudDownloadOptions {
    std::string auth_token;       // 认证令牌
    std::string refresh_token;    // 刷新令牌
    std::string api_key;          // API密钥
    std::string api_secret;       // API密钥
    bool use_vip = false;         // 是否使用VIP加速
    std::string download_thread;  // 线程配置
    int timeout_seconds = 30;     // 超时时间
    int retry_count = 3;          // 重试次数
};

/**
 * @brief 网盘插件接口
 */
class ICloudStoragePlugin {
public:
    virtual ~ICloudStoragePlugin() = default;

    /**
     * @brief 获取平台名称
     */
    virtual std::string platform_name() const = 0;

    /**
     * @brief 获取平台类型
     */
    virtual CloudPlatform platform_type() const = 0;

    /**
     * @brief 检查是否支持该链接
     */
    virtual bool can_handle(const std::string& url) const = 0;

    /**
     * @brief 提取分享链接中的文件信息
     * @param share_url 分享链接
     * @param password 提取密码（可选）
     * @return 提取结果
     */
    virtual CloudExtractionResult extract_share_link(
        const std::string& share_url,
        const std::string& password = ""
    ) = 0;

    /**
     * @brief 获取文件下载链接
     * @param file_id 文件ID
     * @param options 下载选项
     * @return 下载链接
     */
    virtual std::string get_download_url(
        const std::string& file_id,
        const CloudDownloadOptions& options = {}
    ) = 0;

    /**
     * @brief 验证认证信息
     */
    virtual bool authenticate(const std::string& token) = 0;

    /**
     * @brief 获取用户信息
     */
    virtual std::map<std::string, std::string> get_user_info() = 0;

    /**
     * @brief 获取配额信息
     */
    virtual std::map<std::string, size_t> get_quota_info() = 0;
};

/**
 * @brief 网盘链接识别器
 */
class CloudLinkDetector {
public:
    /**
     * @brief 识别网盘平台
     */
    static CloudPlatform detect_platform(const std::string& url);

    /**
     * @brief 提取文件ID或识别码
     */
    static std::string extract_file_id(const std::string& url, CloudPlatform platform);

    /**
     * @brief 规范化URL
     */
    static std::string normalize_url(const std::string& url);

private:
    static std::map<CloudPlatform, std::vector<std::regex>> url_patterns_;
    static void init_patterns();
};

/**
 * @brief 网盘下载插件管理器
 */
class CloudStorageManager {
public:
    CloudStorageManager();
    ~CloudStorageManager();

    /**
     * @brief 注册网盘插件
     */
    void register_plugin(std::unique_ptr<ICloudStoragePlugin> plugin);

    /**
     * @brief 处理分享链接
     */
    CloudExtractionResult handle_share_link(
        const std::string& url,
        const std::string& password = ""
    );

    /**
     * @brief 获取下载链接
     */
    std::string get_direct_download_url(
        const std::string& url,
        const CloudDownloadOptions& options = {}
    );

    /**
     * @brief 获取支持的网盘列表
     */
    std::vector<std::string> get_supported_platforms();

    /**
     * @brief 批量提取
     */
    std::vector<CloudExtractionResult> batch_extract(
        const std::vector<std::string>& urls,
        const std::map<std::string, std::string>& passwords = {}
    );

private:
    class Impl;
    std::unique_ptr<Impl> p_impl;
};

} // namespace falcon