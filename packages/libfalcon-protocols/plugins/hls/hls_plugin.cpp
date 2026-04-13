/**
 * @file hls_plugin.cpp
 * @brief HLS 和 DASH 流媒体协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "hls_plugin.hpp"
#include <falcon/http_plugin.hpp>
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/download_task.hpp>
#include <regex>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace falcon {
namespace plugins {

HLSPlugin::HLSPlugin() {
    FALCON_LOG_INFO("HLS/DASH plugin initialized");
}

std::vector<std::string> HLSPlugin::getSupportedSchemes() const {
    return {"http", "https"};
}

bool HLSPlugin::canHandle(const std::string& url) const {
    // 通过文件扩展名判断
    return url.find(".m3u8") != std::string::npos ||
           url.find(".mpd") != std::string::npos ||
           getStreamType(url) != "unknown";
}

std::unique_ptr<IDownloadTask> HLSPlugin::createTask(const std::string& url,
                                                    const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating HLS/DASH task for: {}", url);

    std::string streamType = getStreamType(url);
    if (streamType == "hls") {
        return createHLSTask(url, options);
    } else if (streamType == "dash") {
        return createDASHTask(url, options);
    } else {
        throw UnsupportedProtocolException("Unknown stream type for URL: " + url);
    }
}

std::string HLSPlugin::getStreamType(const std::string& url) {
    if (isHLSStream(url)) {
        return "hls";
    } else if (isDASHStream(url)) {
        return "dash";
    }
    return "unknown";
}

bool HLSPlugin::isHLSStream(const std::string& url) {
    return url.find(".m3u8") != std::string::npos ||
           url.find("m3u8") != std::string::npos;
}

bool HLSPlugin::isDASHStream(const std::string& url) {
    return url.find(".mpd") != std::string::npos ||
           url.find("dash") != std::string::npos ||
           url.find("mpd") != std::string::npos;
}

std::unique_ptr<IDownloadTask> HLSPlugin::createHLSTask(const std::string& url,
                                                       const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating HLS task for: {}", url);

    // 下载播放列表
    auto httpPlugin = std::make_unique<HttpPlugin>();
    auto m3u8Task = httpPlugin->createTask(url, options);

    // 实际实现需要下载内容并解析
    std::string m3u8Content; // = downloadContent(m3u8Task.get());

    // 解析播放列表
    PlaylistInfo playlist = parseM3U8(m3u8Content, url);

    // 如果是主播放列表，选择最佳质量
    if (!playlist.variants.empty()) {
        std::vector<std::string> streamUrls;
        for (const auto& variant : playlist.variants) {
            streamUrls.push_back(resolveUrl(variant.second, url));
        }

        std::string bestStream = selectBestQuality(streamUrls, options);
        return createHLSTask(bestStream, options);
    }

    // 创建批量下载任务
    std::string outputDir = options.output_path;
    if (outputDir.empty()) {
        outputDir = "./downloads";
    }

    return createBatchTask(playlist.segments, outputDir, options);
}

std::unique_ptr<IDownloadTask> HLSPlugin::createDASHTask(const std::string& url,
                                                        const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating DASH task for: {}", url);

    // 下载MPD清单
    auto httpPlugin = std::make_unique<HttpPlugin>();
    auto mpdTask = httpPlugin->createTask(url, options);

    // 实际实现需要下载内容并解析
    std::string mpdContent; // = downloadContent(mpdTask.get());

    // 解析MPD
    std::vector<DASHAdaptation> adaptations = parseMPD(mpdContent, url);

    // 选择最佳适配集
    // 这里简化处理，选择第一个视频适配集
    DASHAdaptation selectedAdaptation;
    bool found = false;
    for (const auto& adaptation : adaptations) {
        if (adaptation.mimeType.find("video") != std::string::npos) {
            selectedAdaptation = adaptation;
            found = true;
            break;
        }
    }

    if (!found && !adaptations.empty()) {
        selectedAdaptation = adaptations[0];
        found = true;
    }

    if (!found) {
        throw UnsupportedProtocolException("No valid adaptation found in DASH manifest");
    }

    // 选择最佳表现
    DASHRepresentation selectedRep;
    if (!selectedAdaptation.representations.empty()) {
        // 选择带宽适中的表现
        std::sort(selectedAdaptation.representations.begin(),
                  selectedAdaptation.representations.end(),
                  [](const DASHRepresentation& a, const DASHRepresentation& b) {
                      return a.bandwidth < b.bandwidth;
                  });

        // 选择中间带宽的表现
        size_t mid = selectedAdaptation.representations.size() / 2;
        selectedRep = selectedAdaptation.representations[mid];
    }

    // 创建批量下载任务
    std::string outputDir = options.output_path;
    if (outputDir.empty()) {
        outputDir = "./downloads";
    }

    return createBatchTask(selectedRep.segments, outputDir, options);
}

HLSPlugin::PlaylistInfo HLSPlugin::parseM3U8(const std::string& m3u8Content,
                                           const std::string& baseUrl) {
    PlaylistInfo info;
    info.isLive = false;
    info.targetDuration = 0;
    info.version = 1;

    std::istringstream iss(m3u8Content);
    std::string line;

    while (std::getline(iss, line)) {
        // 移除BOM和空白
        line.erase(0, line.find_first_not_of("\xEF\xBB\xBF"));
        line.erase(line.find_last_not_of(" \r\n\t") + 1);

        if (line.empty()) continue;

        if (line == "#EXTM3U") {
            // M3U文件头
            continue;
        } else if (line.find("#EXT-X-VERSION:") == 0) {
            info.version = std::stoul(line.substr(15));
        } else if (line.find("#EXT-X-TARGETDURATION:") == 0) {
            info.targetDuration = std::stod(line.substr(22));
        } else if (line.find("#EXT-X-STREAM-INF:") == 0) {
            // 变体流
            auto attributes = parseStreamInf(line.substr(18));
            std::getline(iss, line);
            if (!line.empty() && line[0] != '#') {
                info.variants[attributes["BANDWIDTH"]] = line;
            }
        } else if (line.find("#EXTINF:") == 0) {
            // 媒体段
            auto extinf = parseExtInf(line);
            std::getline(iss, line);
            if (!line.empty() && line[0] != '#') {
                MediaSegment segment;
                segment.url = resolveUrl(line, baseUrl);
                segment.duration = extinf.first;
                segment.title = extinf.second;
                info.segments.push_back(segment);
            }
        } else if (line.find("#EXT-X-ENDLIST") != std::string::npos) {
            // VOD流结束标记
            info.isLive = false;
        } else if (line.find("#EXT-X-KEY:") == 0) {
            // 加密信息
            // 处理加密
        }
    }

    return info;
}

std::vector<HLSPlugin::DASHAdaptation> HLSPlugin::parseMPD(const std::string& mpdContent,
                                                         const std::string& baseUrl) {
    std::vector<DASHAdaptation> adaptations;

    // 实际实现需要使用XML解析器
    // 这里是简化版本

    // 示例：从MPD中提取适配集信息
    // 需要解析XML结构获取Period、AdaptationSet、Representation等

    return adaptations;
}

std::map<std::string, std::string> HLSPlugin::parseStreamInf(const std::string& attributes) {
    std::map<std::string, std::string> result;

    std::regex attrRegex(R"((\w+)=(?:"([^"]*)"|([^,\s]+)))");
    std::sregex_iterator iter(attributes.begin(), attributes.end(), attrRegex);
    std::sregex_iterator end;

    for (; iter != end; ++iter) {
        std::string key = (*iter)[1].str();
        std::string value = (*iter)[2].str();
        if (value.empty()) {
            value = (*iter)[3].str();
        }
        result[key] = value;
    }

    return result;
}

std::pair<double, std::string> HLSPlugin::parseExtInf(const std::string& extinf) {
    // 格式: #EXTINF:duration,title
    std::regex regex(R"(#EXTINF:([\d.]+)(?:,(.*))?)");
    std::smatch match;

    if (std::regex_match(extinf, match, regex)) {
        double duration = std::stod(match[1].str());
        std::string title = match[2].str();
        return {duration, title};
    }

    return {0, ""};
}

std::vector<std::string> HLSPlugin::downloadMasterPlaylist(const std::string& masterUrl) {
    // 实际实现需要下载并解析主播放列表
    std::vector<std::string> streams;
    return streams;
}

std::string HLSPlugin::selectBestQuality(const std::vector<std::string>& streams,
                                        const DownloadOptions& options) {
    // 根据选项选择最佳质量
    if (!streams.empty()) {
        return streams[0]; // 简化：选择第一个
    }
    return "";
}

std::unique_ptr<IDownloadTask> HLSPlugin::createBatchTask(const std::vector<MediaSegment>& segments,
                                                         const std::string& outputDir,
                                                         const DownloadOptions& options) {
    // 创建批量下载任务
    auto batchTask = std::make_unique<BatchDownloadTask>();

    FALCON_LOG_INFO("Creating batch task with {} segments", segments.size());

    for (const auto& segment : segments) {
        auto httpPlugin = std::make_unique<HttpPlugin>();
        auto segmentTask = httpPlugin->createTask(segment.url, options);

        // 设置段文件名
        std::filesystem::path outputPath(outputDir);
        std::filesystem::path segmentPath(segment.url);
        std::string filename = "segment_" + std::to_string(batchTask->getTaskCount()) +
                              segmentPath.extension().string();

        segmentTask->setFilename((outputPath / filename).string());

        batchTask->addTask(std::move(segmentTask));
    }

    // 设置合并回调
    batchTask->setCompletionCallback([this, outputDir](const std::vector<std::string>& files) {
        std::string outputFile = outputDir + "/output.mp4";
        return mergeSegments(files, outputFile);
    });

    return std::move(batchTask);
}

bool HLSPlugin::mergeSegments(const std::vector<std::string>& segmentFiles,
                             const std::string& outputFile) {
    // 使用ffmpeg或其他工具合并段
    std::string command = "ffmpeg -i \"concat:";

    for (size_t i = 0; i < segmentFiles.size(); ++i) {
        command += segmentFiles[i];
        if (i < segmentFiles.size() - 1) {
            command += "|";
        }
    }

    command += "\" -c copy \"" + outputFile + "\"";

    FALCON_LOG_DEBUG("Merging segments with command: {}", command);

    int result = std::system(command.c_str());

    if (result == 0) {
        FALCON_LOG_INFO("Successfully merged segments to: {}", outputFile);

        // 删除临时段文件
        for (const auto& file : segmentFiles) {
            std::filesystem::remove(file);
        }

        return true;
    } else {
        FALCON_LOG_ERROR("Failed to merge segments");
        return false;
    }
}

std::string HLSPlugin::resolveUrl(const std::string& url, const std::string& baseUrl) {
    if (url.find("://") != std::string::npos) {
        return url; // 绝对URL
    }

    std::filesystem::path basePath(baseUrl);
    std::string baseDir = basePath.parent_path().string();

    if (url[0] == '/') {
        // 相对根路径
        std::filesystem::path parsed(baseUrl);
        return parsed.protocol() + "://" + parsed.host() + url;
    } else {
        // 相对路径
        return baseDir + "/" + url;
    }
}

} // namespace plugins
} // namespace falcon