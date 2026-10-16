[中文](cloud_protocol_design.zh.md) | **English**

# Cloud Storage Protocol Design Comparison

> [!NOTE]
> This document is design material; parts of it may not yet be exposed in the current public API.

## Current Problem

All cloud storage parsers use "magic numbers" to skip over the protocol prefix:

```cpp
// Bad example: current code (error-prone)
size_t bucket_start = 5;  // Why 5? You have to manually count "oss://"
```

**Problems**:
1. The length of `://` must be counted by hand
2. Easy to miscount (as in this bug)
3. Poor readability
4. Hard to maintain

## Solution Comparison

### Option 1: constexpr Constants (Recommended for This Project)

**Advantages**:
- Simple and direct, easy to understand
- Computed at compile time, zero runtime overhead
- Requires no changes to the existing architecture
- Easy to migrate existing code

**Implementation**:
```cpp
namespace falcon::cloud {
    inline constexpr std::string_view PROTOCOL_OSS = "oss://";

    // At the call site
    size_t bucket_start = PROTOCOL_OSS.size();  // automatically = 6
}
```

**Best suited for**:
- Incremental refactoring of the current project
- Situations that must preserve backward compatibility

---

### Option 2: Enum Class + Type Safety

**Advantages**:
- Strongest type safety
- Checked at compile time
- Prevents typos
- Highly extensible

**Implementation**:
```cpp
enum class CloudProtocol : uint8_t {
    S3, OSS, COS, Kodo, Upyun, Unknown
};

// At the call site
auto protocol = CloudProtocol::OSS;
size_t bucket_start = CloudProtocolUtils::get_prefix(protocol).size();
```

**Best suited for**:
- New projects or large-scale refactoring
- Runtime protocol switching
- Passing protocols as configuration parameters

---

### Option 3: Template Metaprogramming (Most General)

**Advantages**:
- Code generated at compile time
- Zero runtime overhead
- Type-safe

**Implementation**:
```cpp
template<std::string_view Protocol>
class CloudUrlParser {
    static auto parse(std::string_view url) {
        constexpr size_t skip = Protocol.size();  // computed at compile time
        // ...
    }
};

using OSSUrlParser = CloudUrlParser<"oss://">;
```

**Best suited for**:
- Generating specialized code per protocol
- Extremely performance-sensitive code

---

## Recommended Implementation Steps

### Phase 1: Immediate (Done)

Fix the current magic-number bug.

### Phase 2: Incremental Refactoring (Recommended)

1. Create `cloud_url_protocols.hpp` defining the constants
2. Gradually migrate existing parsers to `PROTOCOL_XXX.size()`
3. Leave the existing API unchanged

```cpp
// Before migration
size_t bucket_start = 5;  // bad

// After migration
using namespace cloud;
size_t bucket_start = PROTOCOL_OSS.size();  // good
```

### Phase 3: Unified Interface (Optional)

If more protocols need to be added in the future, consider the enum-class approach:

```cpp
enum class CloudProtocol { S3, OSS, COS, ... };

class CloudUrlParser {
    static ParseResult parse(std::string_view url);
    static CloudProtocol detect(std::string_view url);
};
```

---

## Code Examples

### After Refactoring with Option 1

```cpp
#include <falcon/cloud_url_protocols.hpp>

OSSUrl OSSUrlParser::parse(const std::string& url) {
    using namespace cloud;

    if (!starts_with_protocol(url, PROTOCOL_OSS)) {
        return {};
    }

    // Computed automatically — no magic numbers!
    size_t bucket_start = PROTOCOL_OSS.size();
    size_t bucket_end = url.find('/', bucket_start);

    // ... parsing logic
}
```

### After Refactoring with Option 2

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

## Performance Comparison

| Option | Compile-time Cost | Runtime Cost | Code Size |
|--------|------------------|--------------|-----------|
| Current (magic numbers) | None | None | Smallest |
| constexpr constants | None | None | Same |
| Enum class + table lookup | Small | Small | +64 bytes |
| Template metaprogramming | Medium | None | +N bytes per instantiation |

**Conclusion**: The constexpr-constants option strikes the best balance between performance and maintainability.

---

## Test Cases

```cpp
#include <gtest/gtest.h>
#include <falcon/cloud_url_protocols.hpp>

TEST(CloudProtocolTest, ProtocolPrefixes) {
    using namespace cloud;

    EXPECT_EQ(PROTOCOL_S3.size(), 5);
    EXPECT_EQ(PROTOCOL_OSS.size(), 6);
    EXPECT_EQ(PROTOCOL_COS.size(), 6);
    EXPECT_EQ(PROTOCOL_UPYUN.size(), 8);

    // Ensure .size() computes the correct value automatically
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

## Conclusion

**Option 1 (constexpr constants) is recommended**, because:
1. Minimal changes, easy migration
2. Zero runtime overhead
3. Solves the magic-number problem
4. Improves code readability and maintainability

If stronger type safety or runtime protocol switching is needed later, consider migrating to Option 2 (enum class).
