/**
 * @file resume_control.cpp
 * @brief V2 引擎断点续传控制文件实现
 * @author Falcon Team
 * @date 2026-09-13
 */

#include <falcon/protocols/resume_control.hpp>
#include <falcon/logger.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace falcon {

namespace {

constexpr const char* kMagicLine = "falcon-resume-v1";

/// 行式格式解析辅助：按首个 '=' 切分 key=value（URL 查询串可含 '='，
/// 只切第一处）
bool split_key_value(const std::string& line, std::string& key, std::string& value) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
        return false;
    }
    key = line.substr(0, pos);
    value = line.substr(pos + 1);
    return true;
}

bool parse_bytes(const std::string& text, Bytes& out) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long v = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        out = static_cast<Bytes>(v);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::string resume_if_range_value(const ResumeControl& control) {
    return control.etag.empty() ? control.last_modified : control.etag;
}

bool save_resume_control(const std::string& path, const ResumeControl& control) {
    if (path.empty()) {
        return false;
    }

    std::ostringstream out;
    out << kMagicLine << "\n";
    out << "url=" << control.url << "\n";
    out << "total=" << control.total << "\n";
    out << "etag=" << control.etag << "\n";
    out << "last_modified=" << control.last_modified << "\n";
    out << "segments=" << control.segments.size() << "\n";
    for (std::size_t i = 0; i < control.segments.size(); ++i) {
        const auto& seg = control.segments[i];
        out << "seg=" << i << ' ' << seg.offset << ' ' << seg.length << ' '
            << seg.downloaded << "\n";
    }
    const std::string content = out.str();

    // 原子写：先写同目录临时文件再 rename，进程被杀也不会留下半截
    // 控制文件顶着正名（半截文件在 load 的严格校验下也会被拒绝，此处
    // 双保险）
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            FALCON_LOG_WARN_STREAM("续传控制文件写入失败（无法创建）: " << tmp_path);
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file) {
            FALCON_LOG_WARN_STREAM("续传控制文件写入失败: " << tmp_path);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        // 跨设备 rename 等异常场景：退化为直接覆写（控制文件丢失只
        // 影响续传起点，不影响正确性）
        std::filesystem::copy_file(tmp_path, path,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        if (ec) {
            FALCON_LOG_WARN_STREAM("续传控制文件发布失败: " << path
                                  << " (" << ec.message() << ")");
            return false;
        }
    }
    return true;
}

bool load_resume_control(const std::string& path, ResumeControl& control) {
    control = ResumeControl{};

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::string line;
    if (!std::getline(in, line) || line != kMagicLine) {
        return false;
    }

    std::size_t segment_count = 0;
    bool has_total = false;
    control.segments.clear();

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        // seg 行有空格分隔的四元组，先于 key=value 处理
        if (line.rfind("seg=", 0) == 0) {
            std::istringstream iss(line.substr(4));
            std::size_t index = 0;
            unsigned long long offset = 0;
            unsigned long long length = 0;
            unsigned long long downloaded = 0;
            if (!(iss >> index >> offset >> length >> downloaded)) {
                return false;
            }
            ResumeSegment seg;
            seg.offset = static_cast<Bytes>(offset);
            seg.length = static_cast<Bytes>(length);
            seg.downloaded = static_cast<Bytes>(downloaded);
            if (index != control.segments.size()) {
                return false;  // 段序号必须连续递增
            }
            control.segments.push_back(seg);
            continue;
        }

        std::string key;
        std::string value;
        if (!split_key_value(line, key, value)) {
            return false;
        }
        if (key == "url") {
            control.url = value;
        } else if (key == "total") {
            if (!parse_bytes(value, control.total)) {
                return false;
            }
            has_total = true;
        } else if (key == "etag") {
            control.etag = value;
        } else if (key == "last_modified") {
            control.last_modified = value;
        } else if (key == "segments") {
            Bytes count = 0;
            if (!parse_bytes(value, count)) {
                return false;
            }
            segment_count = static_cast<std::size_t>(count);
            control.segments.reserve(segment_count);
        }
        // 未知 key 忽略（向前兼容）
    }

    if (!has_total || control.total == 0 || control.segments.empty()) {
        return false;
    }
    if (control.segments.size() != segment_count) {
        return false;
    }

    // 段计划必须恰好连续覆盖 [0, total)，且进度不越段界
    Bytes expect = 0;
    for (const auto& seg : control.segments) {
        if (seg.length == 0 || seg.offset != expect || seg.downloaded > seg.length) {
            return false;
        }
        expect += seg.length;
    }
    return expect == control.total;
}

void remove_resume_control(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    // 不存在视为成功；其他错误（权限等）忽略——控制文件残留只影响
    // 一次无效的续传尝试，init 的严格校验会拒绝并清理
}

} // namespace falcon
