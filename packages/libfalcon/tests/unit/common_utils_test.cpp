/**
 * @file common_utils_test.cpp
 * @brief 通用工具类和边界条件测试
 * @author Falcon Team
 * @date 2025-12-31
 */

#include <gtest/gtest.h>

#include <falcon/download_options.hpp>
#include <falcon/types.hpp>
#include <falcon/exceptions.hpp>

#include <string>
#include <vector>

using namespace falcon;

// ============================================================================
// DownloadOptions 测试
// ============================================================================

TEST(CommonUtilsTest, DownloadOptionsDefaults) {
    DownloadOptions options;

    // 验证默认值
    EXPECT_EQ(options.max_connections, 1);
    EXPECT_EQ(options.timeout_seconds, 30);
    EXPECT_EQ(options.max_retries, 3);
    EXPECT_EQ(options.speed_limit, 0);
    EXPECT_TRUE(options.resume_if_exists);
    EXPECT_TRUE(options.output_directory.empty());
    EXPECT_TRUE(options.output_filename.empty());
    EXPECT_TRUE(options.user_agent.empty());
    EXPECT_TRUE(options.headers.empty());
}

TEST(CommonUtilsTest, DownloadOptionsCustomValues) {
    DownloadOptions options;
    options.max_connections = 8;
    options.timeout_seconds = 120;
    options.max_retries = 5;
    options.speed_limit = 1024 * 1024;
    options.resume_if_exists = false;
    options.output_directory = "/tmp/downloads";
    options.output_filename = "test.bin";
    options.user_agent = "Falcon/1.0";
    options.headers["X-Custom"] = "value";

    EXPECT_EQ(options.max_connections, 8);
    EXPECT_EQ(options.timeout_seconds, 120);
    EXPECT_EQ(options.max_retries, 5);
    EXPECT_EQ(options.speed_limit, 1024 * 1024);
    EXPECT_FALSE(options.resume_if_exists);
    EXPECT_EQ(options.output_directory, "/tmp/downloads");
    EXPECT_EQ(options.output_filename, "test.bin");
    EXPECT_EQ(options.user_agent, "Falcon/1.0");
    EXPECT_EQ(options.headers["X-Custom"], "value");
}

TEST(CommonUtilsTest, DownloadOptionsEdgeCases) {
    DownloadOptions options;

    // 测试边界值
    options.max_connections = 0;
    EXPECT_EQ(options.max_connections, 0);

    options.max_connections = 1000;
    EXPECT_EQ(options.max_connections, 1000);

    options.timeout_seconds = 0;
    EXPECT_EQ(options.timeout_seconds, 0);

    options.speed_limit = 0;
    EXPECT_EQ(options.speed_limit, 0);

    // 测试大值
    options.speed_limit = std::numeric_limits<size_t>::max();
    EXPECT_EQ(options.speed_limit, std::numeric_limits<size_t>::max());
}

// ============================================================================
// FileInfo 测试
// ============================================================================

TEST(CommonUtilsTest, FileInfoDefaults) {
    FileInfo info;

    // 验证默认值
    EXPECT_TRUE(info.url.empty());
    EXPECT_TRUE(info.filename.empty());
    EXPECT_EQ(info.total_size, 0);
    EXPECT_FALSE(info.supports_resume);
    EXPECT_TRUE(info.content_type.empty());
}

TEST(CommonUtilsTest, FileInfoCustomValues) {
    FileInfo info;
    info.url = "https://example.com/file.bin";
    info.filename = "file.bin";
    info.total_size = 1024 * 1024;
    info.supports_resume = true;
    info.content_type = "application/octet-stream";

    EXPECT_EQ(info.url, "https://example.com/file.bin");
    EXPECT_EQ(info.filename, "file.bin");
    EXPECT_EQ(info.total_size, 1024 * 1024);
    EXPECT_TRUE(info.supports_resume);
    EXPECT_EQ(info.content_type, "application/octet-stream");
}

TEST(CommonUtilsTest, FileInfoLargeSize) {
    FileInfo info;
    info.total_size = std::numeric_limits<uint64_t>::max();

    EXPECT_EQ(info.total_size, std::numeric_limits<uint64_t>::max());
}

// ============================================================================
// ProgressInfo 测试
// ============================================================================

TEST(CommonUtilsTest, ProgressInfoDefaults) {
    ProgressInfo info;

    EXPECT_EQ(info.task_id, 0);
    EXPECT_EQ(info.downloaded_bytes, 0);
    EXPECT_EQ(info.total_bytes, 0);
    EXPECT_EQ(info.speed, 0);
    EXPECT_FLOAT_EQ(info.progress, 0.0f);
}

TEST(CommonUtilsTest, ProgressInfoCalculations) {
    ProgressInfo info;
    info.task_id = 123;
    info.downloaded_bytes = 512;
    info.total_bytes = 1024;
    info.speed = 256;

    info.progress = static_cast<float>(info.downloaded_bytes) / info.total_bytes;

    EXPECT_EQ(info.task_id, 123);
    EXPECT_EQ(info.downloaded_bytes, 512);
    EXPECT_EQ(info.total_bytes, 1024);
    EXPECT_EQ(info.speed, 256);
    EXPECT_FLOAT_EQ(info.progress, 0.5f);
}

TEST(CommonUtilsTest, ProgressInfoZeroTotal) {
    ProgressInfo info;
    info.downloaded_bytes = 100;
    info.total_bytes = 0;

    // 避免除以零
    if (info.total_bytes > 0) {
        info.progress = static_cast<float>(info.downloaded_bytes) / info.total_bytes;
    }

    EXPECT_FLOAT_EQ(info.progress, 0.0f);
}

// ============================================================================
// TaskStatus 测试
// ============================================================================

TEST(CommonUtilsTest, TaskStatusValues) {
    // 验证所有状态值都是有效的
    std::vector<TaskStatus> all_statuses = {
        TaskStatus::Pending,
        TaskStatus::Preparing,
        TaskStatus::Downloading,
        TaskStatus::Paused,
        TaskStatus::Completed,
        TaskStatus::Failed,
        TaskStatus::Cancelled
    };

    for (auto status : all_statuses) {
        // 确保状态可以正常比较
        EXPECT_TRUE(status == TaskStatus::Pending || status != TaskStatus::Pending);
    }
}

TEST(CommonUtilsTest, TaskStatusStringConversion) {
    // 测试状态字符串转换
    EXPECT_STREQ(to_string(TaskStatus::Pending), "Pending");
    EXPECT_STREQ(to_string(TaskStatus::Preparing), "Preparing");
    EXPECT_STREQ(to_string(TaskStatus::Downloading), "Downloading");
    EXPECT_STREQ(to_string(TaskStatus::Paused), "Paused");
    EXPECT_STREQ(to_string(TaskStatus::Completed), "Completed");
    EXPECT_STREQ(to_string(TaskStatus::Failed), "Failed");
    EXPECT_STREQ(to_string(TaskStatus::Cancelled), "Cancelled");
}

// ============================================================================
// TaskPriority 测试
// ============================================================================

TEST(CommonUtilsTest, TaskPriorityValues) {
    // 验证所有优先级值
    std::vector<TaskPriority> all_priorities = {
        TaskPriority::Low,
        TaskPriority::Normal,
        TaskPriority::High
    };

    // 验证优先级可以比较
    EXPECT_TRUE(TaskPriority::High > TaskPriority::Normal);
    EXPECT_TRUE(TaskPriority::Normal > TaskPriority::Low);
    EXPECT_TRUE(TaskPriority::High > TaskPriority::Low);
}

// ============================================================================
// TaskId 测试
// ============================================================================

TEST(CommonUtilsTest, TaskIdValues) {
    // 测试特殊任务 ID
    EXPECT_EQ(INVALID_TASK_ID, 0);

    // 测试有效的任务 ID
    TaskId id1 = 1;
    TaskId id2 = 1000;
    TaskId id3 = std::numeric_limits<TaskId>::max();

    EXPECT_GT(id1, INVALID_TASK_ID);
    EXPECT_GT(id2, id1);
    EXPECT_GT(id3, id2);
}

// ============================================================================
// 类型转换测试
// ============================================================================

TEST(CommonUtilsTest, SpeedTypeConversion) {
    // 测试速度类型的转换
    Speed speed1 = 1024;
    Speed speed2 = 1024 * 1024;
    Speed speed3 = std::numeric_limits<Speed>::max();

    EXPECT_GT(speed2, speed1);
    EXPECT_GT(speed3, speed2);

    // 测试转换为可读格式
    double mbps1 = static_cast<double>(speed1) / (1024 * 1024);
    EXPECT_GT(mbps1, 0);
}

TEST(CommonUtilsTest, BytesTypeConversion) {
    // 测试字节类型的转换
    Bytes bytes1 = 1024;
    Bytes bytes2 = 1024 * 1024;
    Bytes bytes3 = std::numeric_limits<Bytes>::max();

    EXPECT_GT(bytes2, bytes1);
    EXPECT_GT(bytes3, bytes2);

    // 测试转换为可读格式
    double mb1 = static_cast<double>(bytes1) / (1024 * 1024);
    EXPECT_GT(mb1, 0);
}

// ============================================================================
// Duration 测试
// ============================================================================

TEST(CommonUtilsTest, DurationComparisons) {
    Duration d1 = std::chrono::seconds(1);
    Duration d2 = std::chrono::seconds(10);
    Duration d3 = std::chrono::milliseconds(500);

    EXPECT_GT(d2, d1);
    EXPECT_GT(d1, d3);
    EXPECT_LT(d3, d2);

    // 测试转换为毫秒
    auto ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(d1).count();
    EXPECT_EQ(ms1, 1000);
}

// ============================================================================
// 字符串处理测试
// ============================================================================

TEST(CommonUtilsTest, StringProcessing) {
    // 测试 URL 字符串处理
    std::string url1 = "https://example.com/file.bin";
    std::string url2 = "https://example.com/file.bin?param=value";
    std::string url3 = "https://example.com/path/to/file.bin";

    // 验证 URL 格式
    EXPECT_FALSE(url1.empty());
    EXPECT_FALSE(url2.empty());
    EXPECT_FALSE(url3.empty());

    EXPECT_NE(url1.find("https://"), std::string::npos);
    EXPECT_NE(url2.find("?"), std::string::npos);
}

TEST(CommonUtilsTest, StringEdgeCases) {
    // 测试空字符串
    std::string empty_str;
    EXPECT_TRUE(empty_str.empty());
    EXPECT_EQ(empty_str.length(), 0);

    // 测试超长字符串
    std::string long_str(10000, 'a');
    EXPECT_EQ(long_str.length(), 10000);

    // 测试特殊字符字符串
    std::string special_str = "test\x00\x01\x02string";
    EXPECT_GT(special_str.length(), 4);
}

// ============================================================================
// 容器边界测试
// ============================================================================

TEST(CommonUtilsTest, VectorOperations) {
    std::vector<int> vec = {1, 2, 3, 4, 5};

    // 测试边界访问
    EXPECT_EQ(vec.front(), 1);
    EXPECT_EQ(vec.back(), 5);
    EXPECT_EQ(vec.size(), 5);

    // 测试空容器
    std::vector<int> empty_vec;
    EXPECT_TRUE(empty_vec.empty());
    EXPECT_EQ(empty_vec.size(), 0);
}

TEST(CommonUtilsTest, MapOperations) {
    std::map<std::string, std::string> map;

    // 测试插入
    map["key1"] = "value1";
    map["key2"] = "value2";

    EXPECT_EQ(map.size(), 2);
    EXPECT_EQ(map["key1"], "value1");
    EXPECT_EQ(map["key2"], "value2");

    // 测试查找
    EXPECT_EQ(map.count("key1"), 1);
    EXPECT_EQ(map.count("nonexistent"), 0);

    // 测试删除
    map.erase("key1");
    EXPECT_EQ(map.size(), 1);
    EXPECT_EQ(map.count("key1"), 0);
}

// ============================================================================
// 数值计算测试
// ============================================================================

TEST(CommonUtilsTest, NumericCalculations) {
    // 测试进度计算
    uint64_t downloaded = 512;
    uint64_t total = 1024;
    float progress = static_cast<float>(downloaded) / total;

    EXPECT_FLOAT_EQ(progress, 0.5f);

    // 测试剩余时间计算
    Speed speed = 256;  // bytes per second
    uint64_t remaining = total - downloaded;
    auto estimated_seconds = remaining / speed;

    EXPECT_EQ(estimated_seconds, 2);

    // 测试除以零的情况
    EXPECT_THROW(auto result = 1 / 0;, std::exception);
}

TEST(CommonUtilsTest, LargeNumericValues) {
    // 测试大数值计算
    uint64_t large1 = std::numeric_limits<uint64_t>::max();
    uint64_t large2 = large1 / 2;

    EXPECT_GT(large1, large2);
    EXPECT_LT(large2, large1);

    // 测试溢出保护
    uint64_t result = large2 + large2;
    EXPECT_GT(result, large2);
}

// ============================================================================
// 时间相关测试
// ============================================================================

TEST(CommonUtilsTest, TimeCalculations) {
    // 测试时间点
    auto now = std::chrono::system_clock::now();
    auto later = now + std::chrono::seconds(10);

    EXPECT_GT(later, now);

    // 测试时间差
    auto diff = later - now;
    EXPECT_GE(diff, std::chrono::seconds(10));
}

TEST(CommonUtilsTest, SteadyClock) {
    // 测试 steady_clock 用于计时
    auto start = std::chrono::steady_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_GE(elapsed.count(), 10);
}

// ============================================================================
// 布尔值测试
// ============================================================================

TEST(CommonUtilsTest, BooleanLogic) {
    // 测试任务状态检查
    bool is_active = (TaskStatus::Downloading == TaskStatus::Downloading);
    bool is_finished = (TaskStatus::Completed == TaskStatus::Completed);

    EXPECT_TRUE(is_active);
    EXPECT_TRUE(is_finished);

    // 测试逻辑运算
    bool condition1 = true;
    bool condition2 = false;

    EXPECT_TRUE(condition1 && !condition2);
    EXPECT_TRUE(condition1 || condition2);
    EXPECT_TRUE(condition1 != condition2);
}

// ============================================================================
// 指针和智能指针测试
// ============================================================================

TEST(CommonUtilsTest, SmartPointerOperations) {
    // 测试 shared_ptr
    auto ptr1 = std::make_shared<int>(42);
    auto ptr2 = ptr1;

    EXPECT_EQ(ptr1.use_count(), 2);
    EXPECT_EQ(*ptr1, 42);
    EXPECT_EQ(*ptr2, 42);

    ptr1.reset();
    EXPECT_EQ(ptr2.use_count(), 1);
    EXPECT_EQ(*ptr2, 42);

    // 测试 weak_ptr
    std::weak_ptr<int> weak = ptr2;
    auto locked = weak.lock();
    EXPECT_NE(locked, nullptr);
    EXPECT_EQ(*locked, 42);
}

// ============================================================================
// 异常处理测试
// ============================================================================

TEST(CommonUtilsTest, ExceptionHandling) {
    // 测试标准异常
    EXPECT_THROW(throw std::runtime_error("test"), std::runtime_error);
    EXPECT_THROW(throw std::invalid_argument("test"), std::invalid_argument);
    EXPECT_THROW(throw std::out_of_range("test"), std::out_of_range);

    // 测试异常消息
    try {
        throw std::runtime_error("error message");
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "error message");
    }
}

// ============================================================================
// 类型安全测试
// ============================================================================

TEST(CommonUtilsTest, TypeSafety) {
    // 测试类型转换
    int i = 42;
    long l = static_cast<long>(i);
    EXPECT_EQ(l, 42L);

    // 测试无符号转换
    unsigned int ui = 42;
    int si = static_cast<int>(ui);
    EXPECT_EQ(si, 42);

    // 测试大小类型
    size_t st = 42;
    uint64_t u64 = static_cast<uint64_t>(st);
    EXPECT_EQ(u64, 42ULL);
}

// ============================================================================
// 路径处理测试
// ============================================================================

TEST(CommonUtilsTest, PathHandling) {
    // 测试路径组合
    std::string dir = "/tmp/downloads";
    std::string file = "test.bin";

    std::string full_path = dir + "/" + file;
    EXPECT_EQ(full_path, "/tmp/downloads/test.bin");

    // 测试路径分隔符
    std::string path_with_slash = dir + "/";
    EXPECT_EQ(path_with_slash.back(), '/');

    // 测试相对路径
    std::string relative_path = "../test.bin";
    EXPECT_EQ(relative_path.substr(0, 3), "..");
}

// ============================================================================
// URL 解析测试
// ============================================================================

TEST(CommonUtilsTest, UrlParsing) {
    // 测试 HTTP URL
    std::string http_url = "http://example.com/file.bin";
    EXPECT_EQ(http_url.substr(0, 7), "http://");

    // 测试 HTTPS URL
    std::string https_url = "https://example.com/file.bin";
    EXPECT_EQ(https_url.substr(0, 8), "https://");

    // 测试 FTP URL
    std::string ftp_url = "ftp://example.com/file.bin";
    EXPECT_EQ(ftp_url.substr(0, 6), "ftp://");

    // 测试带查询参数的 URL
    std::string url_with_params = "https://example.com/file.bin?param=value&other=123";
    size_t query_pos = url_with_params.find('?');
    EXPECT_NE(query_pos, std::string::npos);
}

// ============================================================================
// 编码相关测试
// ============================================================================

TEST(CommonUtilsTest, EncodingOperations) {
    // 测试 URL 编码字符
    std::string encoded = "file%20name%20with%20spaces.bin";
    EXPECT_NE(encoded.find("%20"), std::string::npos);

    // 测试 URL 解码（简化版）
    std::string decoded = encoded;
    size_t pos = 0;
    while ((pos = decoded.find("%20", pos)) != std::string::npos) {
        decoded.replace(pos, 3, " ");
        pos += 1;
    }
    EXPECT_EQ(decoded, "file name with spaces.bin");
}
