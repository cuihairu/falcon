/**
 * @file bencode.hpp
 * @brief B 编码（Bencoding）编解码器
 * @author Falcon Team
 * @date 2026-05-07
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <stdexcept>

namespace falcon {
namespace protocols {

/**
 * @brief B 编码异常
 */
class BencodeException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief B 编码值类型
 */
enum class BencodeType : uint8_t {
    String,
    Integer,
    List,
    Dict
};

/**
 * @brief B 编码值
 */
class BencodeValue {
public:
    // 构造函数
    BencodeValue() : type_(BencodeType::Integer), intVal_(0) {}
    explicit BencodeValue(int64_t v) : type_(BencodeType::Integer), intVal_(v) {}
    explicit BencodeValue(const std::string& v) : type_(BencodeType::String), strVal_(v) {}
    explicit BencodeValue(const std::vector<BencodeValue>& v) : type_(BencodeType::List), listVal_(v) {}

    // 类型检查
    BencodeType type() const { return type_; }
    bool isString() const { return type_ == BencodeType::String; }
    bool isInt() const { return type_ == BencodeType::Integer; }
    bool isList() const { return type_ == BencodeType::List; }
    bool isDict() const { return type_ == BencodeType::Dict; }

    // 获取值
    const std::string& asString() const;
    int64_t asInt() const;
    const std::vector<BencodeValue>& asList() const;
    const std::map<std::string, BencodeValue>& asDict() const;

    // 设置值
    void setString(const std::string& v);
    void setInt(int64_t v);
    void setList(const std::vector<BencodeValue>& v);
    void setDict(const std::map<std::string, BencodeValue>& v);

    // 字典访问
    BencodeValue& operator[](const std::string& key);
    const BencodeValue& operator[](const std::string& key) const;
    bool hasKey(const std::string& key) const;

    // 列表访问
    BencodeValue& operator[](size_t index);
    const BencodeValue& operator[](size_t index) const;
    size_t size() const;

    // 编码
    std::string encode() const;

    // 解码
    static BencodeValue decode(const std::string& data);
    static BencodeValue decode(const std::string& data, size_t& pos);

private:
    BencodeType type_;
    std::string strVal_;
    int64_t intVal_;
    std::vector<BencodeValue> listVal_;
    std::map<std::string, BencodeValue> dictVal_;
};

/**
 * @brief B 编码工具函数
 */
namespace BencodeUtils {

    /**
     * @brief 编码为 B 编码字符串
     */
    std::string encode(const BencodeValue& value);

    /**
     * @brief 从 B 编码字符串解码
     */
    BencodeValue decode(const std::string& data);

    /**
     * @brief 编码字典（快捷方式）
     */
    std::string encodeDict(const std::map<std::string, std::string>& dict);

    /**
     * @brief 解码为字符串字典（快捷方式）
     */
    std::map<std::string, std::string> decodeDictToStringMap(const BencodeValue& value);

    /**
     * @brief 解码为字符串字典（快捷方式）
     */
    std::map<std::string, std::string> decodeDictToStringMap(const std::string& data);

} // namespace BencodeUtils

} // namespace protocols
} // namespace falcon
