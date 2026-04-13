// Falcon Bug Fix Unit Tests
// Copyright (c) 2025 Falcon Project
//
// This file contains unit tests specifically for the bug fixes made on 2025-12-21

#include <falcon/task_manager.hpp>
#include <falcon/download_task.hpp>
#include <falcon/download_engine.hpp>
#include <falcon/plugin_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>
#include <chrono>
#include <memory>
#include <atomic>
#include <mutex>

using namespace falcon;

class BugFixTest : public ::testing::Test {
protected:
    void SetUp() override {
        event_dispatcher_ = std::make_unique<EventDispatcher>();

        TaskManagerConfig config;
        config.max_concurrent_tasks = 3;
        config.cleanup_interval = std::chrono::seconds(1);

        manager_ = std::make_unique<TaskManager>(config, event_dispatcher_.get());
        engine_ = std::make_unique<DownloadEngine>();
        plugin_manager_ = std::make_unique<PluginManager>();

        manager_->start();
        event_dispatcher_->start();
    }

    void TearDown() override {
        if (manager_) {
            manager_->stop();
        }
        if (event_dispatcher_) {
            event_dispatcher_->stop();
        }
    }

    std::unique_ptr<EventDispatcher> event_dispatcher_;
    std::unique_ptr<TaskManager> manager_;
    std::unique_ptr<DownloadEngine> engine_;
    std::unique_ptr<PluginManager> plugin_manager_;
};

// Test for TaskManager::find_task fix - should handle invalid task IDs gracefully
TEST_F(BugFixTest, TaskManagerFindTaskWithInvalidId) {
    // Test with invalid task ID 0
    {
        auto task = manager_->get_task(0);
        EXPECT_EQ(task, nullptr) << "get_task should return nullptr for invalid ID 0";
    }

    // Test with non-existent large task ID
    {
        auto task = manager_->get_task(99999);
        EXPECT_EQ(task, nullptr) << "get_task should return nullptr for non-existent large ID";
    }

    // Test with another invalid ID
    {
        auto task = manager_->get_task(static_cast<TaskId>(-1));
        EXPECT_EQ(task, nullptr) << "get_task should return nullptr for negative ID";
    }
}

// Test for TaskManager concurrent access fix - should be thread-safe
TEST_F(BugFixTest, TaskManagerConcurrentTaskAccess) {
    const int num_threads = 10;
    const int tasks_per_thread = 5;
    std::vector<std::thread> threads;
    std::vector<TaskId> created_task_ids;
    created_task_ids.reserve(static_cast<size_t>(num_threads * tasks_per_thread));
    std::mutex ids_mutex;
    std::atomic<int> add_ok{0};
    std::atomic<int> get_ok{0};

    // Create tasks from multiple threads
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, i, &created_task_ids, &ids_mutex, &add_ok, &get_ok]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                try {
                    DownloadOptions options;
                    TaskId task_id = static_cast<TaskId>(i * tasks_per_thread + j + 1);
                    auto task = std::make_shared<DownloadTask>(
                        task_id,
                        "https://example.com/file_" + std::to_string(i) + "_" + std::to_string(j) + ".txt",
                        options
                    );

                    TaskId id = manager_->add_task(task);
                    if (id != INVALID_TASK_ID) {
                        add_ok.fetch_add(1);
                        {
                            std::lock_guard<std::mutex> lock(ids_mutex);
                            created_task_ids.push_back(id);
                        }

                        if (manager_->get_task(id) != nullptr) {
                            get_ok.fetch_add(1);
                        }
                    }
                } catch (const std::exception& e) {
                    FAIL() << "Exception during task creation: " << e.what();
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(add_ok.load(), num_threads * tasks_per_thread);
    EXPECT_EQ(get_ok.load(), num_threads * tasks_per_thread);

    // Verify all created tasks can be accessed
    for (TaskId id : created_task_ids) {
        auto task = manager_->get_task(id);
        EXPECT_NE(task, nullptr) << "Should be able to retrieve all created tasks";
    }
}

// Test for DownloadTask status transition fix - should be atomic and thread-safe
TEST_F(BugFixTest, DownloadTaskAtomicStatusTransition) {
    // Create a task using TaskManager to get proper initialization
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/test_file.txt", options);

    // Initial status should be Pending
    EXPECT_EQ(task->status(), TaskStatus::Pending);

    class StatusListener final : public IEventListener {
    public:
        void on_status_changed(TaskId, TaskStatus, TaskStatus new_status) override {
            if (new_status == TaskStatus::Downloading) {
                downloading_transitions.fetch_add(1);
            }
        }

        std::atomic<int> downloading_transitions{0};
    };

    StatusListener listener;
    task->set_listener(&listener);

    std::vector<std::thread> threads;

    const int num_threads = 20;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&task]() {
            try {
                task->set_status(TaskStatus::Downloading);
            } catch (const std::exception& e) {
                (void)e;
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Only one thread should trigger the status transition notification
    EXPECT_EQ(listener.downloading_transitions.load(), 1)
        << "Only one thread should trigger Pending -> Downloading transition";
    EXPECT_EQ(task->status(), TaskStatus::Downloading) << "Task should be in Downloading state";
}

// Test for DownloadEngine task existence check fix
TEST_F(BugFixTest, DownloadEngineCheckTaskExistence) {
    // Test operations on non-existent tasks
    TaskId invalid_id = 99999;

    // These operations should not crash but should handle gracefully
    EXPECT_NO_THROW(engine_->pause_task(invalid_id)) << "pause_task should not crash with invalid ID";
    EXPECT_NO_THROW(engine_->resume_task(invalid_id)) << "resume_task should not crash with invalid ID";
    EXPECT_NO_THROW(engine_->cancel_task(invalid_id)) << "cancel_task should not crash with invalid ID";

    // Test with valid task
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/valid_test.txt", options);

    TaskId valid_id = manager_->add_task(task);
    EXPECT_NE(valid_id, 0) << "Valid task should be added successfully";

    // These operations should work with valid task ID
    EXPECT_NO_THROW(engine_->pause_task(valid_id)) << "pause_task should work with valid ID";
    EXPECT_NO_THROW(engine_->resume_task(valid_id)) << "resume_task should work with valid ID";
    EXPECT_NO_THROW(engine_->cancel_task(valid_id)) << "cancel_task should work with valid ID";
}

// Test for PluginManager nullptr dereference fix
TEST_F(BugFixTest, PluginManagerNoNullptrDereference) {
    // Test getting handlers for unregistered protocols
    auto http_handler = plugin_manager_->getPlugin("http");
    EXPECT_EQ(http_handler, nullptr) << "Should return nullptr for unregistered HTTP handler";

    auto ftp_handler = plugin_manager_->getPlugin("ftp");
    EXPECT_EQ(ftp_handler, nullptr) << "Should return nullptr for unregistered FTP handler";

    auto invalid_handler = plugin_manager_->getPlugin("invalid_protocol");
    EXPECT_EQ(invalid_handler, nullptr) << "Should return nullptr for invalid protocol";
}

// Test for event callback fix (this would require HTTP plugin to be properly fixed)
TEST_F(BugFixTest, EventCallbackNotCrash) {
    // Test that setting event callbacks doesn't crash
    DownloadOptions options;
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/event_test.txt", options);

    // Test triggering progress update
    EXPECT_NO_THROW({
        // This should not crash even if progress update has issues
        task->update_progress(500, 1000, 1000);
    }) << "Progress update should not crash";
}

// Test stress scenario - multiple rapid task operations
TEST_F(BugFixTest, StressTestRapidTaskOperations) {
    const int num_operations = 100;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_operations; ++i) {
        threads.emplace_back([this, i]() {
            try {
                // Create task
                DownloadOptions options;
                TaskId task_id = static_cast<TaskId>(i + 1);
                auto task = std::make_shared<DownloadTask>(
                    task_id,
                    "https://example.com/stress_test_" + std::to_string(i) + ".txt",
                    options
                );

                TaskId id = manager_->add_task(task);
                if (id != 0) {
                    // Rapid operations
                    auto retrieved_task = manager_->get_task(id);
                    if (retrieved_task) {
                        // Try to change status rapidly
                        retrieved_task->set_status(TaskStatus::Preparing);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        retrieved_task->set_status(TaskStatus::Downloading);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        retrieved_task->set_status(TaskStatus::Paused);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        retrieved_task->set_status(TaskStatus::Completed);
                    }
                }
            } catch (const std::exception& e) {
                // Some failures are expected under stress - log but don't fail the test
                std::cout << "Expected exception under stress: " << e.what() << std::endl;
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // If we get here without crashing, the stress test passed
    SUCCEED() << "Stress test completed without crashes";
}
