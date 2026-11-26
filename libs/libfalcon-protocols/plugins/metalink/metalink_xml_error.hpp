/**
 * @file metalink_xml_error.hpp
 * @brief metalink XML 格式解析异常(带行列号)
 * @author Falcon Team
 * @date 2026-09-23
 *
 * XML 格式预检(expat)失败时抛出;语义解析(libmetalink)失败抛
 * std::runtime_error。行列号来自 expat 的错误位置,便于文档诊断。
 */

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace falcon::protocols::metalink {

/**
 * @class XmlParseError
 * @brief 解析失败异常,携带行列号便于诊断
 */
class XmlParseError : public std::runtime_error {
public:
    XmlParseError(const std::string& message, std::size_t line,
                  std::size_t column)
        : std::runtime_error(message + " (line " + std::to_string(line) +
                             ", col " + std::to_string(column) + ")")
        , line_(line)
        , column_(column) {}

    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::size_t column() const noexcept { return column_; }

private:
    std::size_t line_;
    std::size_t column_;
};

} // namespace falcon::protocols::metalink
