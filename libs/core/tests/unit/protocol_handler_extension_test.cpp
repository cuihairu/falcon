// Falcon Protocol Handler Extension Unit Tests

#include <falcon/download_engine.hpp>
#include <falcon/protocol_handler_extension.hpp>
#include <falcon/protocol_registry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// 不实现扩展接口的普通协议处理器
class PlainTestHandler final : public falcon::IProtocolHandler {
public:
    PlainTestHandler(std::string protocol, std::string scheme)
        : protocol_(std::move(protocol)), scheme_(std::move(scheme)) {}

    [[nodiscard]] std::string protocol_name() const override { return protocol_; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {scheme_};
    }
    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind(scheme_ + "://", 0) == 0;
    }
    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "plain";
        return info;
    }
    void download(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr) override {}

private:
    std::string protocol_;
    std::string scheme_;
};

// 实现扩展接口的委托类协议处理器（thunder:// 类插件的实际形态）
class DelegatingTestHandler final : public falcon::IProtocolHandler,
                                    public falcon::IProtocolHandlerExtension {
public:
    DelegatingTestHandler(std::string protocol, std::string scheme)
        : protocol_(std::move(protocol)), scheme_(std::move(scheme)) {}

    [[nodiscard]] std::string protocol_name() const override { return protocol_; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {scheme_};
    }
    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind(scheme_ + "://", 0) == 0;
    }
    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "delegate";
        return info;
    }
    void download(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr) override {}

    void set_protocol_registry(falcon::ProtocolRegistry* registry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_ = registry;
    }

    [[nodiscard]] falcon::ProtocolRegistry* registry() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return registry_;
    }

    // 委托：按目标 URL 的 scheme 经接线的 registry 查目标协议处理器
    // （真实用例中 thunder 解出 http 链接后委托 http handler）
    [[nodiscard]] bool delegate_can_handle(const std::string& url) const {
        auto* reg = registry();
        if (!reg) return false;
        const auto pos = url.find("://");
        if (pos == std::string::npos) return false;
        auto* target = reg->get_handler(url.substr(0, pos));
        return target && target->can_handle(url);
    }

private:
    std::string protocol_;
    std::string scheme_;
    mutable std::mutex mutex_;
    falcon::ProtocolRegistry* registry_ = nullptr;
};

[[nodiscard]] bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// ProtocolHandlerFactory 是裸函数指针，捕获 lambda 不可用——经文件级指针传出创建结果
DelegatingTestHandler* g_created_handler = nullptr;

std::unique_ptr<falcon::IProtocolHandler> make_delegating_handler() {
    auto handler = std::make_unique<DelegatingTestHandler>("factory", "factory");
    g_created_handler = handler.get();
    return handler;
}

} // namespace

TEST(ProtocolHandlerExtensionTest, GetExtensionReturnsNullForPlainHandler) {
    PlainTestHandler handler("plain", "plain");
    EXPECT_EQ(falcon::get_extension(&handler), nullptr);
}

TEST(ProtocolHandlerExtensionTest, GetExtensionResolvesExtensionHandler) {
    DelegatingTestHandler handler("delegate", "delegate");
    auto* ext = falcon::get_extension(&handler);
    ASSERT_NE(ext, nullptr);
    // 返回指针可独立经扩展接口完成接线（无需具体类型）
    falcon::ProtocolRegistry registry;
    ext->set_protocol_registry(&registry);
    EXPECT_EQ(handler.registry(), &registry);
}

TEST(ProtocolHandlerExtensionTest, GetExtensionAcceptsNullHandler) {
    EXPECT_EQ(falcon::get_extension(nullptr), nullptr);
}

TEST(ProtocolHandlerExtensionTest, RegisterHandlerWiresRegistry) {
    falcon::DownloadEngine engine;
    auto handler = std::make_unique<DelegatingTestHandler>("delegate", "delegate");
    auto* raw = handler.get();

    engine.register_handler(std::move(handler));

    // 接线发生在注册时：拿到的 registry 正是收留自己的那张表
    ASSERT_NE(raw->registry(), nullptr);
    EXPECT_EQ(raw->registry()->get_handler("delegate"),
              static_cast<falcon::IProtocolHandler*>(raw));
}

TEST(ProtocolHandlerExtensionTest, RegisteredExtensionDelegatesThroughWiredRegistry) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<PlainTestHandler>("target", "target"));
    auto handler = std::make_unique<DelegatingTestHandler>("delegate", "delegate");
    auto* raw = handler.get();
    engine.register_handler(std::move(handler));

    EXPECT_TRUE(raw->delegate_can_handle("target://example.com/file.bin"));
    EXPECT_FALSE(raw->delegate_can_handle("unregistered://example.com/file.bin"));
}

TEST(ProtocolHandlerExtensionTest, RegisterHandlerFactoryCreatesAndWiresHandler) {
    falcon::DownloadEngine engine;
    g_created_handler = nullptr;
    engine.register_handler_factory(&make_delegating_handler);

    // 工厂注册即生效（旧行为：入队后永不被调用，静默无效）
    ASSERT_NE(g_created_handler, nullptr);
    EXPECT_TRUE(contains(engine.get_supported_protocols(), "factory"));
    EXPECT_TRUE(engine.is_url_supported("factory://example.com/file.bin"));

    // 工厂创建的委托类处理器同样完成扩展接线
    ASSERT_NE(g_created_handler->registry(), nullptr);
    EXPECT_EQ(g_created_handler->registry()->get_handler("factory"),
              static_cast<falcon::IProtocolHandler*>(g_created_handler));
}

TEST(ProtocolHandlerExtensionTest, RegisterHandlerFactoryRejectsNullAndEmpty) {
    falcon::DownloadEngine engine;
    engine.register_handler_factory(nullptr);
    engine.register_handler_factory(
        []() -> std::unique_ptr<falcon::IProtocolHandler> { return nullptr; });
    engine.register_handler(nullptr);

    EXPECT_TRUE(engine.get_supported_protocols().empty());
}
