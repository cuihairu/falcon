/**
 * @file bencode.cpp
 * @brief B 编码（Bencoding）编解码器实现
 * @author Falcon Team
 * @date 2026-05-07
 */

#include "bencode.hpp"
#include <sstream>
#include <stdexcept>
#include <cctype>

namespace falcon {
namespace protocols {

//==============================================================================
// BencodeValue
//==============================================================================

const std::string& BencodeValue::asString() const {
    if (type_ != BencodeType::String) {
        throw BencodeException("Value is not a string");
    }
    return strVal_;
}

int64_t BencodeValue::asInt() const {
    if (type_ != BencodeType::Integer) {
        throw BencodeException("Value is not an integer");
    }
    return intVal_;
}

const std::vector<BencodeValue>& BencodeValue::asList() const {
    if (type_ != BencodeType::List) {
        throw BencodeException("Value is not a list");
    }
    return listVal_;
}

const std::map<std::string, BencodeValue>& BencodeValue::asDict() const {
    if (type_ != BencodeType::Dict) {
        throw BencodeException("Value is not a dict");
    }
    return dictVal_;
}

void BencodeValue::setString(const std::string& v) {
    type_ = BencodeType::String;
    strVal_ = v;
}

void BencodeValue::setInt(int64_t v) {
    type_ = BencodeType::Integer;
    intVal_ = v;
}

void BencodeValue::setList(const std::vector<BencodeValue>& v) {
    type_ = BencodeType::List;
    listVal_ = v;
}

void BencodeValue::setDict(const std::map<std::string, BencodeValue>& v) {
    type_ = BencodeType::Dict;
    dictVal_ = v;
}

BencodeValue& BencodeValue::operator[](const std::string& key) {
    if (type_ != BencodeType::Dict) {
        type_ = BencodeType::Dict;
    }
    return dictVal_[key];
}

const BencodeValue& BencodeValue::operator[](const std::string& key) const {
    if (type_ != BencodeType::Dict) {
        throw BencodeException("Value is not a dict");
    }
    auto it = dictVal_.find(key);
    if (it == dictVal_.end()) {
        throw BencodeException("Key not found: " + key);
    }
    return it->second;
}

bool BencodeValue::hasKey(const std::string& key) const {
    if (type_ != BencodeType::Dict) {
        return false;
    }
    return dictVal_.find(key) != dictVal_.end();
}

BencodeValue& BencodeValue::operator[](size_t index) {
    if (type_ != BencodeType::List) {
        throw BencodeException("Value is not a list");
    }
    if (index >= listVal_.size()) {
        throw BencodeException("Index out of range");
    }
    return listVal_[index];
}

const BencodeValue& BencodeValue::operator[](size_t index) const {
    if (type_ != BencodeType::List) {
        throw BencodeException("Value is not a list");
    }
    if (index >= listVal_.size()) {
        throw BencodeException("Index out of range");
    }
    return listVal_[index];
}

size_t BencodeValue::size() const {
    switch (type_) {
        case BencodeType::String: return strVal_.size();
        case BencodeType::List: return listVal_.size();
        case BencodeType::Dict: return dictVal_.size();
        case BencodeType::Integer: return 0;
    }
    return 0;
}

std::string BencodeValue::encode() const {
    return BencodeUtils::encode(*this);
}

BencodeValue BencodeValue::decode(const std::string& data) {
    size_t pos = 0;
    return decode(data, pos);
}

BencodeValue BencodeValue::decode(const std::string& data, size_t& pos) {
    if (pos >= data.size()) {
        throw BencodeException("Unexpected end of data");
    }

    char ch = data[pos];

    if (std::isdigit(static_cast<unsigned char>(ch))) {
        // 字符串: length:string
        size_t colonPos = data.find(':', pos);
        if (colonPos == std::string::npos) {
            throw BencodeException("Invalid string format");
        }

        size_t length = std::stoul(data.substr(pos, colonPos - pos));
        pos = colonPos + 1;

        if (pos + length > data.size()) {
            throw BencodeException("String length exceeds data size");
        }

        std::string result = data.substr(pos, length);
        pos += length;
        return BencodeValue(result);

    } else if (ch == 'i') {
        // 整数: i<integer>e
        size_t endPos = data.find('e', pos);
        if (endPos == std::string::npos) {
            throw BencodeException("Invalid integer format");
        }

        std::string intStr = data.substr(pos + 1, endPos - pos - 1);
        pos = endPos + 1;

        try {
            return BencodeValue(static_cast<int64_t>(std::stoll(intStr)));
        } catch (const std::exception&) {
            throw BencodeException("Invalid integer value: " + intStr);
        }

    } else if (ch == 'l') {
        // 列表: l<contents>e
        pos++;
        std::vector<BencodeValue> result;

        while (pos < data.size() && data[pos] != 'e') {
            result.push_back(decode(data, pos));
        }

        if (pos >= data.size()) {
            throw BencodeException("Unterminated list");
        }
        pos++; // 跳过 'e'
        return BencodeValue(result);

    } else if (ch == 'd') {
        // 字典: d<key,value pairs>e
        pos++;
        std::map<std::string, BencodeValue> result;

        while (pos < data.size() && data[pos] != 'e') {
            BencodeValue key = decode(data, pos);
            if (!key.isString()) {
                throw BencodeException("Dictionary key must be string");
            }

            BencodeValue value = decode(data, pos);
            result[key.asString()] = value;
        }

        if (pos >= data.size()) {
            throw BencodeException("Unterminated dict");
        }
        pos++; // 跳过 'e'

        BencodeValue dictVal;
        dictVal.setDict(result);
        return dictVal;

    } else {
        throw BencodeException(std::string("Unknown bencode type: ") + ch);
    }
}

//==============================================================================
// BencodeUtils
//==============================================================================

namespace BencodeUtils {

std::string encode(const BencodeValue& value) {
    std::ostringstream oss;

    switch (value.type()) {
        case BencodeType::String:
            oss << value.asString().size() << ':' << value.asString();
            break;

        case BencodeType::Integer:
            oss << 'i' << value.asInt() << 'e';
            break;

        case BencodeType::List: {
            oss << 'l';
            for (const auto& item : value.asList()) {
                oss << encode(item);
            }
            oss << 'e';
            break;
        }

        case BencodeType::Dict: {
            oss << 'd';
            for (const auto& pair : value.asDict()) {
                oss << pair.first.size() << ':' << pair.first;
                oss << encode(pair.second);
            }
            oss << 'e';
            break;
        }
    }

    return oss.str();
}

BencodeValue decode(const std::string& data) {
    size_t pos = 0;
    return BencodeValue::decode(data, pos);
}

std::string encodeDict(const std::map<std::string, std::string>& dict) {
    std::ostringstream oss;
    oss << 'd';
    for (const auto& pair : dict) {
        oss << pair.first.size() << ':' << pair.first;
        oss << pair.second.size() << ':' << pair.second;
    }
    oss << 'e';
    return oss.str();
}

std::map<std::string, std::string> decodeDictToStringMap(const BencodeValue& value) {
    std::map<std::string, std::string> result;

    if (!value.isDict()) {
        throw BencodeException("Value is not a dict");
    }

    for (const auto& pair : value.asDict()) {
        if (pair.second.isString()) {
            result[pair.first] = pair.second.asString();
        } else {
            result[pair.first] = encode(pair.second);
        }
    }

    return result;
}

std::map<std::string, std::string> decodeDictToStringMap(const std::string& data) {
    BencodeValue value = decode(data);
    return decodeDictToStringMap(value);
}

} // namespace BencodeUtils

} // namespace protocols
} // namespace falcon
