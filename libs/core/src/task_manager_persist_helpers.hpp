#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/protocol_handler.hpp>
#include "internal/thread_pool.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <unordered_set>
#include <unordered_map>

namespace falcon {

namespace {
constexpr int kTaskManagerStateVersion = 5;
constexpr const char* kTaskManagerStateMagic = "falcon_task_state";

template <typename Int>
bool read_int(std::istream& in, Int& out) {
    long long value = 0;
    if (!(in >> value)) {
        return false;
    }
    out = static_cast<Int>(value);
    return true;
}

TaskPriority sanitize_priority_for_restore(int priority_int) {
    if (priority_int < static_cast<int>(TaskPriority::Low) ||
        priority_int > static_cast<int>(TaskPriority::Critical)) {
        return TaskPriority::Normal;
    }
    return static_cast<TaskPriority>(priority_int);
}

TaskStatus sanitize_status_for_restore(TaskStatus status) {
    // Change active statuses to Paused for safety
    // This prevents auto-starting downloads when loading from state
    if (status == TaskStatus::Downloading || status == TaskStatus::Preparing) {
        return TaskStatus::Paused;
    }
    return status;
}

/**
 * @brief 从流中读取下载选项
 *
 * version >= 3 才读取 auto_file_renaming、version >= 4 才读取
 * conditional_get、version >= 5 才读取客户端证书尾字段——旧版本
 * 状态文件按默认值（false / 空串）解析，保证升级后旧档可继续恢复
 */
bool read_download_options(std::istream& in, int version, DownloadOptions& options) {
    if (!(in >> options.max_connections >> options.timeout_seconds >> options.max_retries >>
          options.retry_delay_seconds)) {
        return false;
    }
    if (!(in >> std::quoted(options.output_directory) >> std::quoted(options.output_filename))) {
        return false;
    }
    if (!(in >> options.speed_limit)) {
        return false;
    }

    int resume_enabled = 1;
    if (!read_int(in, resume_enabled)) {
        return false;
    }
    options.resume_enabled = resume_enabled != 0;

    auto read_quoted = [&in](std::string& value) {
        in >> std::quoted(value);
        return static_cast<bool>(in);
    };
    if (!read_quoted(options.user_agent)) return false;
    if (!read_quoted(options.proxy)) return false;
    if (!read_quoted(options.proxy_type)) return false;
    if (!read_quoted(options.proxy_username)) return false;
    if (!read_quoted(options.proxy_password)) return false;

    int verify_ssl = 1;
    if (!read_int(in, verify_ssl)) {
        return false;
    }
    options.verify_ssl = verify_ssl != 0;

    if (!(in >> std::quoted(options.referer) >> std::quoted(options.cookie_file) >>
          std::quoted(options.cookie_jar))) {
        return false;
    }

    if (!(in >> options.min_segment_size)) {
        return false;
    }

    int adaptive = 1;
    if (!read_int(in, adaptive)) {
        return false;
    }
    options.adaptive_segment_sizing = adaptive != 0;

    if (!(in >> options.progress_interval_ms)) {
        return false;
    }

    int create_dir = 1;
    int overwrite = 0;
    if (!read_int(in, create_dir) || !read_int(in, overwrite)) {
        return false;
    }
    options.create_directory = create_dir != 0;
    options.overwrite_existing = overwrite != 0;

    if (version >= 3) {
        int auto_rename = 0;
        if (!read_int(in, auto_rename)) {
            return false;
        }
        options.auto_file_renaming = auto_rename != 0;
    }

    if (version >= 4) {
        int conditional = 0;
        if (!read_int(in, conditional)) {
            return false;
        }
        options.conditional_get = conditional != 0;
    }

    // version >= 5 才读取客户端证书尾字段——旧版本状态文件按默认值
    //（空串 = 无客户端证书）解析
    if (version >= 5) {
        if (!(in >> std::quoted(options.client_certificate) >>
              std::quoted(options.client_private_key))) {
            return false;
        }
    }

    std::size_t header_count = 0;
    if (!(in >> header_count)) {
        return false;
    }
    for (std::size_t i = 0; i < header_count; ++i) {
        std::string k;
        std::string v;
        if (!(in >> std::quoted(k) >> std::quoted(v))) {
            return false;
        }
        options.headers.emplace(std::move(k), std::move(v));
    }

    return true;
}

/**
 * @brief 从流中读取单个任务数据
 */
bool read_task_data(std::istream& in, int version, TaskId& id, std::string& url,
                     std::string& output_path, std::string& error_message,
                     Bytes& downloaded_bytes, Bytes& total_bytes,
                     BytesPerSecond& speed, int& status_int, int& priority_int) {
    if (!(in >> id >> status_int >> downloaded_bytes >> total_bytes >> speed)) {
        return false;
    }
    if (version >= 2) {
        if (!(in >> priority_int)) {
            return false;
        }
    } else {
        priority_int = static_cast<int>(TaskPriority::Normal);
    }
    if (!(in >> std::quoted(url) >> std::quoted(output_path) >> std::quoted(error_message))) {
        return false;
    }
    return true;
}
} // namespace
} // namespace falcon
