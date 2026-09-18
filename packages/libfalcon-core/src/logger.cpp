// Falcon 日志后端实现
//
// 历史上这些函数全部 inline 定义在 logger.hpp：每个包含该头的编译
// 单元都生成一份弱符号实例，而链接器只保留一份机器码——覆盖数据
// 按实例分账，执行计数只落在被选中的那一个 TU，其余 TU 的同名函数
// 永远显示未覆盖（46 个生产 TU × 未命中实例 ≈ 127 行测量水分）。
// 现集中到单个 .cpp：实例唯一、账目唯一，header 只留声明与模板。
// 行为零变化（函数局部 static 本就是全程序唯一，见 [basic.link]）。

#include <falcon/logger.hpp>

#ifdef FALCON_USE_SPDLOG

#include <cstdio>
#include <cstring>

namespace falcon {

spdlog::level::level_enum to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::Off:   return spdlog::level::off;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Warn:  return spdlog::level::warn;
        case LogLevel::Info:  return spdlog::level::info;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Trace: return spdlog::level::trace;
    }
    return spdlog::level::info;
}

void FalconConsoleSink::sink_it_(const spdlog::details::log_msg& msg) {
    FILE* out = msg.level >= spdlog::level::warn ? stderr : stdout;
    const char* tag = level_tag(msg.level);
    std::fwrite(tag, 1, std::strlen(tag), out);
    if (msg.payload.size() > 0) {
        std::fwrite(msg.payload.data(), 1, msg.payload.size(), out);
    }
    std::fputc('\n', out);
    std::fflush(out);
}

void FalconConsoleSink::flush_() {
    std::fflush(stdout);
    std::fflush(stderr);
}

const char* FalconConsoleSink::level_tag(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace:    return "[TRACE] ";
        case spdlog::level::debug:    return "[DEBUG] ";
        case spdlog::level::info:     return "[INFO] ";
        case spdlog::level::warn:     return "[WARN] ";
        case spdlog::level::err:      return "[ERROR] ";
        case spdlog::level::critical: return "[CRITICAL] ";
        default:                      return "[OFF] ";
    }
}

std::shared_ptr<spdlog::logger>& falcon_logger_storage() {
    static std::shared_ptr<spdlog::logger> instance = [] {
        auto logger = std::make_shared<spdlog::logger>(
            "falcon", std::make_shared<FalconConsoleSink>());
        logger->set_level(to_spdlog_level(get_log_level()));
        return logger;
    }();
    return instance;
}

const std::shared_ptr<spdlog::logger>& falcon_logger() {
    return falcon_logger_storage();
}

void set_log_level(LogLevel level) {
    global_log_level_storage().store(static_cast<int>(level), std::memory_order_relaxed);
    if (auto logger = falcon_logger_storage()) {
        logger->set_level(to_spdlog_level(level));
    }
}

void set_log_level(int level) {
    global_log_level_storage().store(level, std::memory_order_relaxed);
    if (auto logger = falcon_logger_storage()) {
        if (level < 0) {
            logger->set_level(spdlog::level::off);
        } else if (level > static_cast<int>(LogLevel::Trace)) {
            logger->set_level(spdlog::level::trace);
        } else {
            logger->set_level(to_spdlog_level(static_cast<LogLevel>(level)));
        }
    }
}

void log_info(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Info)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::info, "{}", msg);
    }
}

void log_debug(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Debug)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::debug, "{}", msg);
    }
}

void log_warn(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Warn)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::warn, "{}", msg);
    }
}

void log_error(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Error)) {
        return;
    }
    if (auto logger = falcon_logger_storage()) {
        logger->log(spdlog::level::err, "{}", msg);
    }
}

} // namespace falcon

#else  // FALCON_USE_SPDLOG
//==============================================================================
// 内置回退后端（无 spdlog 的最小构建使用；行为与 spdlog 后端一致）
//==============================================================================

namespace falcon {

void set_log_level(LogLevel level) {
    global_log_level_storage().store(static_cast<int>(level), std::memory_order_relaxed);
}

void set_log_level(int level) {
    global_log_level_storage().store(level, std::memory_order_relaxed);
}

void log_info(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Info)) {
        return;
    }
    std::cout << "[INFO] " << msg << std::endl;
}

void log_debug(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Debug)) {
        return;
    }
    std::cout << "[DEBUG] " << msg << std::endl;
}

void log_warn(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Warn)) {
        return;
    }
    std::cerr << "[WARN] " << msg << std::endl;
}

void log_error(const std::string& msg) {
    if (static_cast<int>(get_log_level()) < static_cast<int>(LogLevel::Error)) {
        return;
    }
    std::cerr << "[ERROR] " << msg << std::endl;
}

} // namespace falcon

#endif  // FALCON_USE_SPDLOG

namespace falcon {
namespace detail {

std::string format_log_message_core(const std::string& format,
                                    const std::string* replacements,
                                    std::size_t replacement_count) {
    std::string result;
    result.reserve(format.size() + replacement_count * 8);

    size_t arg_index = 0;
    size_t i = 0;
    while (i < format.size()) {
        if (format[i] == '{') {
            const size_t close = format.find('}', i + 1);
            if (close != std::string::npos) {
                if (arg_index < replacement_count) {
                    result += replacements[arg_index++];
                } else {
                    result.append(format, i, close - i + 1);
                }
                i = close + 1;
                continue;
            }
        }

        result.push_back(format[i]);
        ++i;
    }

    for (; arg_index < replacement_count; ++arg_index) {
        if (!result.empty() && result.back() != ' ') {
            result.push_back(' ');
        }
        result += replacements[arg_index];
    }

    return result;
}

std::string to_log_string(const std::string& value) {
    return value;
}

std::string to_log_string(const char* value) {
    return value == nullptr ? std::string("(null)") : std::string(value);
}

std::string to_log_string(char* value) {
    return value == nullptr ? std::string("(null)") : std::string(value);
}

} // namespace detail
} // namespace falcon
