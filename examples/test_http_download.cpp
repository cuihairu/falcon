/**
 * @file test_http_download.cpp
 * @brief Test program for HTTP download functionality
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>

using namespace falcon;
using namespace std;

/**
 * @brief Simple event listener for testing
 */
class TestEventListener : public IEventListener {
public:
    void on_status_changed(TaskId task_id, TaskStatus old_status, TaskStatus new_status) override {
        cout << "[Task " << task_id << "] Status: " << to_string(old_status)
             << " -> " << to_string(new_status) << endl;
    }

    void on_progress(const ProgressInfo& info) override {
        cout << "\r[Task " << info.task_id << "] Progress: "
             << fixed << setprecision(1) << (info.progress * 100) << "%";
        if (info.total_bytes > 0) {
            cout << " (" << info.downloaded_bytes / 1024 << "KB / "
                 << info.total_bytes / 1024 << "KB)";
        }
        if (info.speed > 0) {
            cout << " @ " << info.speed / 1024 << "KB/s";
        }
        cout.flush();
    }

    void on_error(TaskId task_id, const std::string& error_message) override {
        cout << "\n[Task " << task_id << "] Error: " << error_message << endl;
    }

    void on_completed(TaskId task_id, const std::string& output_path) override {
        cout << "\n[Task " << task_id << "] Completed: " << output_path << endl;
    }

    void on_file_info(TaskId task_id, const FileInfo& info) override {
        cout << "[Task " << task_id << "] File info:" << endl;
        cout << "  Size: " << info.total_size << " bytes" << endl;
        cout << "  Filename: " << info.filename << endl;
        cout << "  Content-Type: " << info.content_type << endl;
        cout << "  Supports Resume: " << (info.supports_resume ? "Yes" : "No") << endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <URL> [output_file]" << endl;
        cout << "Example: " << argv[0] << " https://httpbin.org/json test.json" << endl;
        return 1;
    }

    string url = argv[1];
    string output_file = argc > 2 ? argv[2] : "";

    try {
        // Create download engine
        cout << "Initializing Falcon Download Engine..." << endl;
        DownloadEngine engine;

        // List supported protocols
        auto protocols = engine.get_supported_protocols();
        cout << "Supported protocols: ";
        for (const auto& proto : protocols) {
            cout << proto << " ";
        }
        cout << endl;

        // Check if URL is supported
        if (!engine.is_url_supported(url)) {
            cerr << "Error: URL not supported: " << url << endl;
            return 1;
        }

        // Configure download options
        DownloadOptions options;
        options.max_connections = 4;
        options.timeout_seconds = 30;
        options.resume_enabled = true;
        options.verify_ssl = true;
        options.user_agent = "Falcon/0.1 Test";

        if (!output_file.empty()) {
            options.output_filename = output_file;
        }

        // Add event listener
        TestEventListener listener;
        engine.add_listener(&listener);

        // Create download task
        cout << "\nStarting download: " << url << endl;
        auto task = engine.add_task(url, options);

        if (!task) {
            cerr << "Error: Failed to create download task" << endl;
            return 1;
        }

        if (!engine.start_task(task->id())) {
            cerr << "Error: Failed to start download task" << endl;
            return 1;
        }

        // Wait for download to complete
        while (!task->is_finished()) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }

        // Check final status
        if (task->status() == TaskStatus::Completed) {
            cout << "\n\nDownload completed successfully!" << endl;
            auto progress = task->get_progress_info();
            cout << "Total downloaded: " << progress.downloaded_bytes << " bytes" << endl;
            cout << "Output file: " << task->output_path() << endl;
            return 0;
        } else if (task->status() == TaskStatus::Failed) {
            cerr << "\n\nDownload failed: " << task->error_message() << endl;
            return 1;
        } else if (task->status() == TaskStatus::Cancelled) {
            cerr << "\n\nDownload was cancelled" << endl;
            return 1;
        }

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
