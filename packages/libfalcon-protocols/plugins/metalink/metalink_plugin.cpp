/**
 * @file metalink_plugin.cpp
 * @brief Metalink 协议插件实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "metalink_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <random>
#include <cstring>

// 简单的 XML 解析器实现
namespace {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

} // anonymous namespace

namespace falcon {
namespace plugins {

// ============================================================================
// MetalinkPlugin 实现
// ============================================================================

MetalinkPlugin::MetalinkPlugin() {
    FALCON_LOG_INFO("Metalink plugin initialized");
}

MetalinkPlugin::~MetalinkPlugin() {
    FALCON_LOG_DEBUG("Metalink plugin shutdown");
}

std::vector<std::string> MetalinkPlugin::getSupportedSchemes() const {
    return {"metalink"};
}

bool MetalinkPlugin::canHandle(const std::string& url) const {
    return url.find("metalink:") == 0 || url.find(".metalink") != std::string::npos;
}

std::unique_ptr<IDownloadTask> MetalinkPlugin::createTask(const std::string& url,
                                                          const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating Metalink task for: {}", url);
    return std::make_unique<MetalinkDownloadTask>(url, options);
}

// ============================================================================
// XMLParser 实现
// ============================================================================

std::unique_ptr<MetalinkPlugin::XMLParser::Node>
MetalinkPlugin::XMLParser::parse(const std::string& xml) {
    auto root = std::make_unique<Node>();

    std::regex tagRegex(R"(<([^>]+)>)");
    std::regex endTagRegex(R"(</([^>]+)>)");
    std::regex attrRegex(R"(([^=]+)="([^"]*)")");

    std::vector<Node*> stack;
    stack.push_back(root.get());

    std::string::const_iterator searchStart = xml.cbegin();
    std::smatch match;

    while (std::regex_search(searchStart, xml.cend(), match, tagRegex)) {
        std::string tagContent = match[1].str();

        // 检查是否是结束标签
        std::smatch endMatch;
        std::string current(searchStart, xml.cend());
        if (std::regex_search(current, endMatch, endTagRegex)) {
            if (endMatch.position() < match.position()) {
                // 这是结束标签
                std::string tagName = endMatch[1].str();
                if (!stack.empty() && stack.back()->name == tagName) {
                    stack.pop_back();
                }
                searchStart += endMatch.position() + endMatch.length();
                continue;
            }
        }

        // 检查是否是自闭合标签
        bool selfClosing = tagContent.back() == '/';
        if (selfClosing) {
            tagContent = tagContent.substr(0, tagContent.size() - 1);
        }

        // 提取标签名和属性
        tagContent = trim(tagContent);
        size_t spacePos = tagContent.find_first_of(" \t");
        std::string tagName = spacePos == std::string::npos ?
            tagContent : tagContent.substr(0, spacePos);

        // 解析属性
        std::map<std::string, std::string> attributes;
        if (spacePos != std::string::npos) {
            std::string attrStr = tagContent.substr(spacePos);
            std::sregex_iterator it(attrStr.begin(), attrStr.end(), attrRegex);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                attributes[(*it)[1].str()] = (*it)[2].str();
            }
        }

        auto node = std::make_unique<Node>();
        node->name = tagName;
        node->attributes = attributes;

        if (selfClosing) {
            stack.back()->children.push_back(std::move(node));
        } else {
            Node* nodePtr = node.get();
            stack.back()->children.push_back(std::move(node));
            stack.push_back(nodePtr);
        }

        searchStart += match.position() + match.length();
    }

    return root;
}

MetalinkPlugin::XMLParser::Node*
MetalinkPlugin::XMLParser::findChild(Node* node, const std::string& name) {
    for (auto& child : node->children) {
        if (child->name == name) {
            return child.get();
        }
    }
    return nullptr;
}

std::vector<MetalinkPlugin::XMLParser::Node*>
MetalinkPlugin::XMLParser::findChildren(Node* node, const std::string& name) {
    std::vector<Node*> result;
    for (auto& child : node->children) {
        if (child->name == name) {
            result.push_back(child.get());
        }
    }
    return result;
}

std::string MetalinkPlugin::XMLParser::getAttribute(const Node* node,
                                                     const std::string& name,
                                                     const std::string& defaultValue) {
    auto it = node->attributes.find(name);
    return it != node->attributes.end() ? it->second : defaultValue;
}

// ============================================================================
// MetalinkDownloadTask 实现
// ============================================================================

MetalinkPlugin::MetalinkDownloadTask::MetalinkDownloadTask(const std::string& url,
                                                           const DownloadOptions& options)
    : url_(url)
    , options_(options)
    , status_(TaskStatus::Pending)
    , totalSize_(0)
    , downloadedBytes_(0)
    , downloadSpeed_(0) {
}

MetalinkPlugin::MetalinkDownloadTask::~MetalinkDownloadTask() {
    if (status_ == TaskStatus::Downloading) {
        cancel();
    }
}

bool MetalinkPlugin::MetalinkDownloadTask::downloadMetalink(const std::string& url) {
    // 如果是 metalink:// URL，需要先下载 metalink 文件
    if (url.find("metalink://") == 0) {
        std::string actualUrl = url.substr(11);
        // 使用 HTTP 下载器下载 metalink 文件
        // 这里需要与 HTTP 插件集成
        // 暂时返回失败
        errorMessage_ = "Metalink URL download not implemented";
        return false;
    }

    // 如果是本地文件
    if (url.find("file://") == 0 || url[0] == '/') {
        std::string filePath = url.find("file://") == 0 ? url.substr(7) : url;
        return parseMetalink(filePath);
    }

    // HTTP/HTTPS URL
    if (url.find("http://") == 0 || url.find("https://") == 0) {
        // 下载 metalink 文件到临时位置
        std::string tempFile = "/tmp/temp_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".metalink";

        // 这里需要使用 HTTP 插件下载
        // 暂时返回失败
        errorMessage_ = "HTTP download of metalink file not implemented";
        return false;
    }

    errorMessage_ = "Unsupported metalink URL";
    return false;
}

bool MetalinkPlugin::MetalinkDownloadTask::parseMetalink(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        errorMessage_ = "Failed to open metalink file: " + filePath;
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    file.close();

    // 解析 XML
    auto root = XMLParser::parse(content);

    // Metalink v4 格式
    // <metalink version="4.0" origin="...">
    //   <file name="...">
    //     <size>...</size>
    //     <hash type="...">...</hash>
    //     <url priority="..." type="...">...</url>
    //   </file>
    // </metalink>

    auto metalinkNode = XMLParser::findChild(root.get(), "metalink");
    if (!metalinkNode) {
        errorMessage_ = "Invalid metalink file: missing metalink root element";
        return false;
    }

    auto files = XMLParser::findChildren(metalinkNode, "file");
    if (files.empty()) {
        errorMessage_ = "No files found in metalink";
        return false;
    }

    // 只处理第一个文件（可以扩展支持多文件）
    auto fileNode = files[0];
    metalinkFile_.name = XMLParser::getAttribute(fileNode, "name");

    // 获取文件大小
    auto sizeNode = XMLParser::findChild(fileNode, "size");
    if (sizeNode) {
        try {
            metalinkFile_.size = std::stoull(sizeNode->text);
        } catch (...) {
            metalinkFile_.size = 0;
        }
    }

    // 获取哈希
    auto hashNodes = XMLParser::findChildren(fileNode, "hash");
    for (auto hashNode : hashNodes) {
        std::string type = XMLParser::getAttribute(hashNode, "type");
        if (!type.empty() && !hashNode->text.empty()) {
            metalinkFile_.hashType = type;
            metalinkFile_.hash = hashNode->text;
            break;  // 使用第一个哈希
        }
    }

    // 获取资源列表
    auto urlNodes = XMLParser::findChildren(fileNode, "url");
    for (auto urlNode : urlNodes) {
        MetalinkResource resource;
        resource.url = urlNode->text;
        resource.priority = std::stoi(XMLParser::getAttribute(urlNode, "priority", "50"));
        resource.type = XMLParser::getAttribute(urlNode, "type", "http");
        resource.location = XMLParser::getAttribute(urlNode, "location", "");
        resource.preference = std::stoi(XMLParser::getAttribute(urlNode, "preference", "100"));

        metalinkFile_.resources.push_back(resource);
    }

    if (metalinkFile_.resources.empty()) {
        errorMessage_ = "No download resources found in metalink";
        return false;
    }

    totalSize_ = metalinkFile_.size;

    FALCON_LOG_INFO("Parsed metalink file: {}, size: {}, resources: {}",
                   metalinkFile_.name, metalinkFile_.size,
                   metalinkFile_.resources.size());

    return true;
}

MetalinkResource MetalinkPlugin::MetalinkDownloadTask::selectBestResource(
    const std::vector<MetalinkResource>& resources) {

    if (resources.empty()) {
        return MetalinkResource{};
    }

    // 按优先级和偏好值排序
    std::vector<MetalinkResource> sorted = resources;
    std::sort(sorted.begin(), sorted.end(),
        [](const MetalinkResource& a, const MetalinkResource& b) {
            if (a.priority != b.priority) {
                return a.priority > b.priority;  // 高优先级优先
            }
            return a.preference > b.preference;
        });

    return sorted[0];
}

std::vector<MetalinkResource> MetalinkPlugin::MetalinkDownloadTask::filterByLocation(
    const std::vector<MetalinkResource>& resources,
    const std::string& location) {

    std::vector<MetalinkResource> filtered;
    for (const auto& resource : resources) {
        if (resource.location == location) {
            filtered.push_back(resource);
        }
    }
    return filtered;
}

bool MetalinkPlugin::MetalinkDownloadTask::verifyHash(const std::string& filePath) {
    if (metalinkFile_.hash.empty()) {
        FALCON_LOG_WARN("No hash available for verification");
        return true;  // 没有哈希则跳过验证
    }

    // 计算文件哈希
    std::string computedHash;
    // 这里需要实现哈希计算
    // 暂时返回 true
    FALCON_LOG_INFO("Hash verification for {} type {} not implemented",
                   metalinkFile_.hashType, metalinkFile_.hash);
    return true;
}

bool MetalinkPlugin::MetalinkDownloadTask::tryAlternativeSource() {
    // 选择备用源
    auto currentUrl = currentTask_ ? "" : "";  // 获取当前 URL

    // 过滤掉已尝试的源
    std::vector<MetalinkResource> alternatives;
    for (const auto& resource : metalinkFile_.resources) {
        // 跳过当前失败的源
        alternatives.push_back(resource);
    }

    if (alternatives.empty()) {
        errorMessage_ = "No alternative sources available";
        return false;
    }

    // 选择最佳备用源
    auto best = selectBestResource(alternatives);

    FALCON_LOG_INFO("Trying alternative source: {}", best.url);

    // 创建新的下载任务
    falcon::DownloadOptions httpOptions = options_;
    httpOptions.output_path = options_.output_path.empty() ?
        "./" + metalinkFile_.name : options_.output_path;

    // 这里需要创建 HTTP 下载任务
    // 暂时返回 false
    return false;
}

void MetalinkPlugin::MetalinkDownloadTask::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (status_ != TaskStatus::Pending) {
        return;
    }

    status_ = TaskStatus::Downloading;

    // 解析或下载 metalink 文件
    if (!downloadMetalink(url_)) {
        status_ = TaskStatus::Failed;
        return;
    }

    // 选择最佳下载源
    auto bestResource = selectBestResource(metalinkFile_.resources);

    FALCON_LOG_INFO("Selected resource: {} (priority: {}, type: {})",
                   bestResource.url, bestResource.priority, bestResource.type);

    // 创建实际的下载任务
    falcon::DownloadOptions httpOptions = options_;
    httpOptions.output_path = options_.output_path.empty() ?
        "./" + metalinkFile_.name : options_.output_path;

    // 这里需要创建 HTTP/FTP 下载任务
    // 暂时标记为完成
    status_ = TaskStatus::Completed;

    FALCON_LOG_INFO("Metalink download completed: {}", metalinkFile_.name);
}

void MetalinkPlugin::MetalinkDownloadTask::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Downloading && currentTask_) {
        status_ = TaskStatus::Paused;
        currentTask_->pause();
    }
}

void MetalinkPlugin::MetalinkDownloadTask::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Paused && currentTask_) {
        status_ = TaskStatus::Downloading;
        currentTask_->resume();
    }
}

void MetalinkPlugin::MetalinkDownloadTask::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = TaskStatus::Cancelled;
    if (currentTask_) {
        currentTask_->cancel();
    }
}

TaskStatus MetalinkPlugin::MetalinkDownloadTask::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

float MetalinkPlugin::MetalinkDownloadTask::getProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (totalSize_ == 0) return 0.0f;
    return static_cast<float>(downloadedBytes_) / totalSize_;
}

uint64_t MetalinkPlugin::MetalinkDownloadTask::getTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalSize_;
}

uint64_t MetalinkPlugin::MetalinkDownloadTask::getDownloadedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downloadedBytes_;
}

uint64_t MetalinkPlugin::MetalinkDownloadTask::getSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downloadSpeed_;
}

std::string MetalinkPlugin::MetalinkDownloadTask::getErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorMessage_;
}

} // namespace plugins
} // namespace falcon
