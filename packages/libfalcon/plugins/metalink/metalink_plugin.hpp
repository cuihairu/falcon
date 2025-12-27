/**
 * @file metalink_plugin.hpp
 * @brief Metalink 协议插件
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <map>

namespace falcon {
namespace plugins {

/**
 * @struct MetalinkResource
 * @brief Metalink 资源信息
 */
struct MetalinkResource {
    std::string url;                    // 资源 URL
    int priority;                       // 优先级 (1-100, 100 最高)
    std::string type;                   // 类型 (http, ftp, etc.)
    std::string location;               // 地理位置 (如 CN, US)
    size_t preference;                  // 偏好值
};

/**
 * @struct MetalinkFile
 * @brief Metalink 文件信息
 */
struct MetalinkFile {
    std::string name;                   // 文件名
    std::vector<MetalinkResource> resources;  // 下载源列表
    uint64_t size;                      // 文件大小
    std::string hash;                   // 哈希值
    std::string hashType;               // 哈希算法 (md5, sha1, sha256)
};

/**
 * @class MetalinkPlugin
 * @brief Metalink 协议处理器
 *
 * 支持 Metalink v4 文件格式
 * 提供多源下载、镜像选择、校验和验证功能
 */
class MetalinkPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    MetalinkPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~MetalinkPlugin();

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "metalink"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief Metalink 下载任务
     */
    class MetalinkDownloadTask : public IDownloadTask {
    public:
        MetalinkDownloadTask(const std::string& url, const DownloadOptions& options);
        virtual ~MetalinkDownloadTask();

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
        std::string url_;
        DownloadOptions options_;
        MetalinkFile metalinkFile_;

        TaskStatus status_;
        std::string errorMessage_;

        // 当前下载任务
        std::unique_ptr<IDownloadTask> currentTask_;

        // 统计信息
        uint64_t totalSize_;
        uint64_t downloadedBytes_;
        uint64_t downloadSpeed_;

        // 线程安全
        mutable std::mutex mutex_;

        /**
         * @brief 解析 Metalink 文件
         */
        bool parseMetalink(const std::string& filePath);

        /**
         * @brief 从 URL 下载 Metalink 文件
         */
        bool downloadMetalink(const std::string& url);

        /**
         * @brief 选择最佳下载源
         */
        MetalinkResource selectBestResource(const std::vector<MetalinkResource>& resources);

        /**
         * @brief 根据地理位置选择资源
         */
        std::vector<MetalinkResource> filterByLocation(const std::vector<MetalinkResource>& resources,
                                                       const std::string& location);

        /**
         * @brief 验证文件哈希
         */
        bool verifyHash(const std::string& filePath);

        /**
         * @brief 尝试备用源
         */
        bool tryAlternativeSource();
    };

    /**
     * @brief XML 解析辅助类
     */
    class XMLParser {
    public:
        struct Node {
            std::string name;
            std::string text;
            std::map<std::string, std::string> attributes;
            std::vector<std::unique_ptr<Node>> children;
        };

        /**
         * @brief 解析 XML 字符串
         */
        static std::unique_ptr<Node> parse(const std::string& xml);

        /**
         * @brief 查找子节点
         */
        static Node* findChild(Node* node, const std::string& name);

        /**
         * @brief 查找所有子节点
         */
        static std::vector<Node*> findChildren(Node* node, const std::string& name);

        /**
         * @brief 获取属性值
         */
        static std::string getAttribute(const Node* node, const std::string& name,
                                       const std::string& defaultValue = "");
    };
};

} // namespace plugins
} // namespace falcon
