// Falcon Download Engine Unit Tests

#include <falcon/download_engine.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace {

class TestProtocolHandler final : public falcon::IProtocolHandler {
public:
    [[nodiscard]] std::string protocol_name() const override { return "test"; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override { return {"test"}; }

    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind("test://", 0) == 0;
    }

    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "file.bin";
        info.total_size = 4;
        info.supports_resume = true;
        info.content_type = "application/octet-stream";
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        if (!task) return;

        task->mark_started();

        auto info = get_file_info(task->url(), task->options());
        task->set_file_info(info);

        task->update_progress(2, 4, 2);
        task->update_progress(4, 4, 2);

        task->set_status(falcon::TaskStatus::Completed);
    }

    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr task) override {
        if (task) task->set_status(falcon::TaskStatus::Cancelled);
    }
};

} // namespace

TEST(DownloadEngineTest, AddTaskBuildsOutputPath) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.output_directory = "downloads";
    options.output_filename = "out.bin";

    auto task = engine.add_task("test://example.com/path/file.bin", options);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->output_path(), "downloads/out.bin");
}

TEST(DownloadEngineTest, AddTaskExtractsFilenameWhenNotProvided) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    auto task = engine.add_task("test://example.com/path/file.bin", options);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->output_path(), "file.bin");
}

TEST(DownloadEngineTest, AddTaskDefaultsWhenNoPathSegment) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    auto task = engine.add_task("test://example.com/path/", options);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->output_path(), "download");
}

TEST(DownloadEngineTest, UnsupportedUrlThrows) {
    falcon::DownloadEngine engine;
    EXPECT_THROW(engine.add_task("noscheme", falcon::DownloadOptions{}), falcon::UnsupportedProtocolException);
}

TEST(DownloadEngineTest, StartTaskCompletesWithTestHandler) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/path/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(1)));
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
}

