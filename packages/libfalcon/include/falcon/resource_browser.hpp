/**
 * @file resource_browser.hpp
 * @brief 远程资源浏览器接口定义
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <chrono>

namespace falcon {

/**
 * @brief 资源类型枚举
 */
enum class ResourceType {
    Unknown,
    File,           // 文件
    Directory,      // 目录
    Symlink,        // 符号链接
    BlockDevice,    // 块设备
    CharDevice,     // 字符设备
    FIFO,           // 命名管道
    Socket          // 套接字
};

/**
 * @brief 文件权限信息
 */
struct FilePermissions {
    bool owner_read : 1;
    bool owner_write : 1;
    bool owner_execute : 1;
    bool group_read : 1;
    bool group_write : 1;
    bool group_execute : 1;
    bool other_read : 1;
    bool other_write : 1;
    bool other_execute : 1;

    // 转换为权限字符串（如 rwxr-xr-x）
    std::string to_string() const;

    // 从八进制权限创建（如 755）
    static FilePermissions from_octal(int mode);
};

/**
 * @brief 远程资源项信息
 */
struct RemoteResource {
    std::string name;              // 资源名称
    std::string path;              // 完整路径
    ResourceType type;             // 资源类型
    uint64_t size = 0;             // 文件大小（字节）
    FilePermissions permissions;    // 权限信息
    std::string owner;             // 所有者
    std::string group;             // 所属组
    std::string modified_time;     // 修改时间
    std::string created_time;      // 创建时间
    std::string accessed_time;     // 访问时间
    std::string mime_type;         // MIME类型
    std::string etag;              // ETag（用于HTTP/WebDAV）
    std::map<std::string, std::string> metadata; // 额外元数据

    // 符号链接目标
    std::string symlink_target;

    // 缩略显示
    std::string display_name() const;

    // 格式化大小显示
    std::string formatted_size() const;

    // 是否为目录
    bool is_directory() const { return type == ResourceType::Directory; }

    // 是否为文件
    bool is_file() const { return type == ResourceType::File; }
};

/**
 * @brief 列表选项
 */
struct ListOptions {
    bool show_hidden = false;       // 显示隐藏文件
    bool recursive = false;         // 递归列表
    int max_depth = 0;              // 最大递归深度（0=无限制）
    std::string sort_by = "name";  // 排序字段：name/size/modified_time
    bool sort_desc = false;         // 降序排序
    bool include_metadata = false;  // 包含详细元数据
    std::string filter;             // 文件过滤（通配符）
};

/**
 * @brief 资源浏览器接口
 */
class IResourceBrowser {
public:
    virtual ~IResourceBrowser() = default;

    /**
     * @brief 获取浏览器名称
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief 获取支持的协议
     */
    virtual std::vector<std::string> get_supported_protocols() const = 0;

    /**
     * @brief 检查是否支持该URL
     */
    virtual bool can_handle(const std::string& url) const = 0;

    /**
     * @brief 连接到远程资源
     */
    virtual bool connect(const std::string& url,
                         const std::map<std::string, std::string>& options = {}) = 0;

    /**
     * @brief 断开连接
     */
    virtual void disconnect() = 0;

    /**
     * @brief 列出目录内容
     */
    virtual std::vector<RemoteResource> list_directory(
        const std::string& path,
        const ListOptions& options = {}
    ) = 0;

    /**
     * @brief 获取资源信息
     */
    virtual RemoteResource get_resource_info(const std::string& path) = 0;

    /**
     * @brief 创建目录
     */
    virtual bool create_directory(const std::string& path,
                                   bool recursive = false) = 0;

    /**
     * @brief 删除文件或目录
     */
    virtual bool remove(const std::string& path, bool recursive = false) = 0;

    /**
     * @brief 重命名或移动资源
     */
    virtual bool rename(const std::string& old_path,
                         const std::string& new_path) = 0;

    /**
     * @brief 复制资源
     */
    virtual bool copy(const std::string& source_path,
                      const std::string& dest_path) = 0;

    /**
     * @brief 检查路径是否存在
     */
    virtual bool exists(const std::string& path) = 0;

    /**
     * @brief 获取当前工作目录
     */
    virtual std::string get_current_directory() = 0;

    /**
     * @brief 切换工作目录
     */
    virtual bool change_directory(const std::string& path) = 0;

    /**
     * @brief 获取根目录
     */
    virtual std::string get_root_path() = 0;

    /**
     * @brief 获取存储配额信息
     */
    virtual std::map<std::string, uint64_t> get_quota_info() = 0;
};

/**
 * @brief 资源浏览器管理器
 */
class ResourceBrowserManager {
public:
    ResourceBrowserManager();
    ~ResourceBrowserManager();

    /**
     * @brief 注册浏览器
     */
    void register_browser(std::unique_ptr<IResourceBrowser> browser);

    /**
     * @brief 浏览远程资源
     */
    std::vector<RemoteResource> browse(
        const std::string& url,
        const std::map<std::string, std::string>& options = {}
    );

    /**
     * @brief 浏览指定路径
     */
    std::vector<RemoteResource> browse_path(
        const std::string& url,
        const std::string& path,
        const std::map<std::string, std::string>& options = {}
    );

    /**
     * @brief 获取资源信息
     */
    RemoteResource get_info(const std::string& url,
                             const std::string& path = "");

    /**
     * @brief 获取支持的协议列表
     */
    std::vector<std::string> get_supported_protocols();

    /**
     * @brief 获取格式化的目录列表（用于CLI显示）
     */
    std::string format_listing(const std::vector<RemoteResource>& resources,
                               bool long_format = false);

    /**
     * @brief 获取JSON格式的目录列表（用于Web界面）
     */
    std::string format_json_listing(const std::vector<RemoteResource>& resources);

private:
    class Impl;
    std::unique_ptr<Impl> p_impl_;
};

/**
 * @brief CLI格式化工具
 */
class BrowserFormatter {
public:
    /**
     * @brief 短格式（类似ls -l）
     */
    static std::string format_short(const std::vector<RemoteResource>& resources);

    /**
     * @brief 长格式（包含所有信息）
     */
    static std::string format_long(const std::vector<RemoteResource>& resources);

    /**
     * @brief 树形格式（递归显示）
     */
    static std::string format_tree(const std::vector<RemoteResource>& resources,
                                   const std::string& base_path = "",
                                   int max_depth = 0);

    /**
     * @brief 表格格式
     */
    static std::string format_table(const std::vector<RemoteResource>& resources);

    /**
     * @brief 自定义列格式
     */
    static std::string format_custom(
        const std::vector<RemoteResource>& resources,
        const std::vector<std::string>& columns
    );
};

/**
 * @brief 浏览器工厂
 */
class BrowserFactory {
public:
    /**
     * @brief 创建浏览器实例
     */
    static std::unique_ptr<IResourceBrowser> create_browser(const std::string& protocol);

    /**
     * @brief 从URL创建浏览器
     */
    static std::unique_ptr<IResourceBrowser> create_from_url(const std::string& url);

    /**
     * @brief 注册浏览器类型
     */
    template<typename T>
    static void register_browser(const std::string& protocol);
};

} // namespace falcon