# 云存储协议设计对比

## 当前问题

所有云存储解析器都使用"魔法数字"来跳过协议前缀：

```cpp
// ❌ 当前代码（容易出错）
size_t bucket_start = 5;  // 为什么是 5？需要手动数 "oss://"
```

**问题**：
1. 需要手动计算 `://` 的长度
2. 容易数错（如本次 bug）
3. 代码可读性差
4. 难以维护

## 解决方案对比

### 方案 1：constexpr 常量（推荐用于当前项目）

**优点**：
- ✅ 简单直接，易于理解
- ✅ 编译期计算，零运行时开销
- ✅ 不改变现有架构
- ✅ 容易迁移现有代码

**实现**：
```cpp
namespace falcon::cloud {
    inline constexpr std::string_view PROTOCOL_OSS = "oss://";

    // 使用时
    size_t bucket_start = PROTOCOL_OSS.size();  // 自动 = 6
}
```

**适用场景**：
- 当前项目的渐进式重构
- 需要保持向后兼容

---

### 方案 2：枚举类 + 类型安全

**优点**：
- ✅ 最强的类型安全
- ✅ 编译期检查
- ✅ 避免拼写错误
- ✅ 可扩展性好

**实现**：
```cpp
enum class CloudProtocol : uint8_t {
    S3, OSS, COS, Kodo, Upyun, Unknown
};

// 使用时
auto protocol = CloudProtocol::OSS;
size_t bucket_start = CloudProtocolUtils::get_prefix(protocol).size();
```

**适用场景**：
- 新项目或大规模重构
- 需要运行时协议切换
- 协议作为配置参数

---

### 方案 3：模板元编程（最通用）

**优点**：
- ✅ 编译期生成代码
- ✅ 零运行时开销
- ✅ 类型安全

**实现**：
```cpp
template<std::string_view Protocol>
class CloudUrlParser {
    static auto parse(std::string_view url) {
        constexpr size_t skip = Protocol.size();  // 编译期计算
        // ...
    }
};

using OSSUrlParser = CloudUrlParser<"oss://">;
```

**适用场景**：
- 需要为每个协议生成特化代码
- 性能极度敏感的代码

---

## 推荐实施步骤

### 阶段 1：立即可行（已完成）

✅ 修复当前的魔法数字 bug

### 阶段 2：渐进式重构（推荐）

1. 创建 `cloud_url_protocols.hpp` 定义常量
2. 逐步迁移现有解析器使用 `PROTOCOL_XXX.size()`
3. 保持现有 API 不变

```cpp
// 迁移前
size_t bucket_start = 5;  // ❌

// 迁移后
using namespace cloud;
size_t bucket_start = PROTOCOL_OSS.size();  // ✅
```

### 阶段 3：统一接口（可选）

如果未来需要添加更多协议，考虑使用枚举类方案：

```cpp
enum class CloudProtocol { S3, OSS, COS, ... };

class CloudUrlParser {
    static ParseResult parse(std::string_view url);
    static CloudProtocol detect(std::string_view url);
};
```

---

## 代码示例

### 使用方案 1 重构后

```cpp
#include <falcon/cloud_url_protocols.hpp>

OSSUrl OSSUrlParser::parse(const std::string& url) {
    using namespace cloud;

    if (!starts_with_protocol(url, PROTOCOL_OSS)) {
        return {};
    }

    // 自动计算，无需魔法数字！
    size_t bucket_start = PROTOCOL_OSS.size();
    size_t bucket_end = url.find('/', bucket_start);

    // ... 解析逻辑
}
```

### 使用方案 2 重构后

```cpp
#include <falcon/cloud_protocol.hpp>

std::pair<std::string, std::string> parse_s3_url(std::string_view url) {
    auto protocol = CloudProtocolUtils::detect_from_url(url);

    if (protocol != CloudProtocol::S3) {
        throw std::invalid_argument("Not an S3 URL");
    }

    return CloudProtocolUtils::parse_bucket_and_key(url, protocol);
}
```

---

## 性能对比

| 方案 | 编译期开销 | 运行时开销 | 代码大小 |
|------|-----------|-----------|---------|
| 当前（魔法数字） | 无 | 无 | 最小 |
| constexpr 常量 | 无 | 无 | 相同 |
| 枚举类 + 表查找 | 小 | 小 | +64 bytes |
| 模板元编程 | 中 | 无 | 每个实例化 +N bytes |

**结论**：constexpr 常量方案在性能和可维护性之间达到最佳平衡。

---

## 测试用例

```cpp
#include <gtest/gtest.h>
#include <falcon/cloud_url_protocols.hpp>

TEST(CloudProtocolTest, ProtocolPrefixes) {
    using namespace cloud;

    EXPECT_EQ(PROTOCOL_S3.size(), 5);
    EXPECT_EQ(PROTOCOL_OSS.size(), 6);
    EXPECT_EQ(PROTOCOL_COS.size(), 6);
    EXPECT_EQ(PROTOCOL_UPYUN.size(), 8);

    // 确保 .size() 自动正确计算
    EXPECT_EQ(PROTOCOL_OSS, "oss://");
    EXPECT_EQ(PROTOCOL_OSS.size(), std::string("oss://").size());
}

TEST(CloudProtocolTest, ParseBucket) {
    using namespace cloud;

    std::string url = "oss://mybucket/path/to/file.txt";
    size_t bucket_start = PROTOCOL_OSS.size();  // = 6

    EXPECT_EQ(url.substr(bucket_start, 8), "mybucket");
}
```

---

## 结论

**推荐使用方案 1（constexpr 常量）**，因为：
1. 最小改动，易于迁移
2. 零运行时开销
3. 解决了魔法数字问题
4. 提高代码可读性和可维护性

如果未来需要更强的类型安全或运行时协议切换，可以考虑迁移到方案 2（枚举类）。
