/**
 * @file task_storage.cpp
 * @brief SQLite-based task persistence implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "task_storage.hpp"
#include <falcon/logger.hpp>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <mutex>
#include <sstream>
#include <iomanip>

namespace falcon::daemon {

// SQL schema version
constexpr int SCHEMA_VERSION = 1;

// SQL statements
constexpr const char* CREATE_TASKS_TABLE = R"sql(
CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY,
    url TEXT NOT NULL,
    output_path TEXT,
    status INTEGER NOT NULL DEFAULT 0,
    progress REAL NOT NULL DEFAULT 0.0,
    total_bytes INTEGER NOT NULL DEFAULT 0,
    downloaded_bytes INTEGER NOT NULL DEFAULT 0,
    speed INTEGER NOT NULL DEFAULT 0,
    error_message TEXT,
    options_json TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    completed_at INTEGER
);
)sql";

constexpr const char* CREATE_INDEX_STATUS = R"sql(
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
)sql";

constexpr const char* CREATE_INDEX_CREATED = R"sql(
CREATE INDEX IF NOT EXISTS idx_tasks_created ON tasks(created_at);
)sql";

/**
 * @brief Implementation class (PIMPL pattern)
 */
class TaskStorage::Impl {
public:
    explicit Impl(const TaskStorageConfig& config) : config_(config), db_(nullptr) {}

    ~Impl() {
        close();
    }

    bool initialize() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (db_) return true;  // Already initialized

        // Open database
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        int rc = sqlite3_open_v2(config_.db_path.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }

        // Set busy timeout
        sqlite3_busy_timeout(db_, config_.busy_timeout_ms);

        // Enable WAL mode for better concurrency
        if (config_.enable_wal_mode) {
            exec_sql("PRAGMA journal_mode=WAL;");
        }

        // Create tables
        if (config_.auto_create_tables) {
            if (!create_tables()) {
                close();
                return false;
            }
        }

        return true;
    }

    bool is_ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return db_ != nullptr;
    }

    TaskId create_task(const TaskRecord& record) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return INVALID_TASK_ID;

        // Serialize options to JSON
        nlohmann::json options_json = serialize_options(record.options);

        auto created_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            record.created_at.time_since_epoch()).count();
        auto updated_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            record.updated_at.time_since_epoch()).count();

        const char* sql = R"sql(
            INSERT INTO tasks (url, output_path, status, progress, total_bytes,
                               downloaded_bytes, speed, error_message, options_json,
                               created_at, updated_at, completed_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return INVALID_TASK_ID;
        }

        sqlite3_bind_text(stmt, 1, record.url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.output_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, static_cast<int>(record.status));
        sqlite3_bind_double(stmt, 4, record.progress);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(record.total_bytes));
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(record.downloaded_bytes));
        sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(record.speed));
        sqlite3_bind_text(stmt, 8, record.error_message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, options_json.dump().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 10, created_ts);
        sqlite3_bind_int64(stmt, 11, updated_ts);
        sqlite3_bind_int64(stmt, 12, record.completed_at.has_value() ?
            std::chrono::duration_cast<std::chrono::milliseconds>(
                record.completed_at->time_since_epoch()).count() : 0);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return INVALID_TASK_ID;
        }

        TaskId id = static_cast<TaskId>(sqlite3_last_insert_rowid(db_));
        return id;
    }

    std::optional<TaskRecord> get_task(TaskId id) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return std::nullopt;

        const char* sql = R"sql(
            SELECT id, url, output_path, status, progress, total_bytes,
                   downloaded_bytes, speed, error_message, options_json,
                   created_at, updated_at, completed_at
            FROM tasks WHERE id = ?;
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return std::nullopt;
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));

        std::optional<TaskRecord> result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = parse_task_record(stmt);
        }

        sqlite3_finalize(stmt);
        return result;
    }

    bool update_task(TaskId id, const TaskRecord& record) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        nlohmann::json options_json = serialize_options(record.options);

        auto updated_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            record.updated_at.time_since_epoch()).count();
        auto completed_ts = record.completed_at.has_value() ?
            std::chrono::duration_cast<std::chrono::milliseconds>(
                record.completed_at->time_since_epoch()).count() : 0;

        const char* sql = R"sql(
            UPDATE tasks SET
                url = ?, output_path = ?, status = ?, progress = ?,
                total_bytes = ?, downloaded_bytes = ?, speed = ?,
                error_message = ?, options_json = ?, updated_at = ?, completed_at = ?
            WHERE id = ?;
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_text(stmt, 1, record.url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.output_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, static_cast<int>(record.status));
        sqlite3_bind_double(stmt, 4, record.progress);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(record.total_bytes));
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(record.downloaded_bytes));
        sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(record.speed));
        sqlite3_bind_text(stmt, 8, record.error_message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, options_json.dump().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 10, updated_ts);
        sqlite3_bind_int64(stmt, 11, completed_ts);
        sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(id));

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        return sqlite3_changes(db_) > 0;
    }

    bool delete_task(TaskId id) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        const char* sql = "DELETE FROM tasks WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        return sqlite3_changes(db_) > 0;
    }

    bool task_exists(TaskId id) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        const char* sql = "SELECT 1 FROM tasks WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
        bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);

        return exists;
    }

    std::vector<TaskRecord> list_tasks(const TaskQueryOptions& options) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<TaskRecord> results;
        if (!db_) return results;

        std::ostringstream sql;
        sql << "SELECT id, url, output_path, status, progress, total_bytes, "
            << "downloaded_bytes, speed, error_message, options_json, "
            << "created_at, updated_at, completed_at FROM tasks";

        if (options.status_filter.has_value()) {
            sql << " WHERE status = " << static_cast<int>(*options.status_filter);
        }

        sql << " ORDER BY created_at " << (options.order_by_created_desc ? "DESC" : "ASC");

        if (options.limit > 0) {
            sql << " LIMIT " << options.limit;
            if (options.offset > 0) {
                sql << " OFFSET " << options.offset;
            }
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return results;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(parse_task_record(stmt));
        }

        sqlite3_finalize(stmt);
        return results;
    }

    std::vector<TaskRecord> get_tasks_by_status(TaskStatus status) const {
        TaskQueryOptions options;
        options.status_filter = status;
        return list_tasks(options);
    }

    std::vector<TaskRecord> get_active_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<TaskRecord> results;
        if (!db_) return results;

        const char* sql = R"sql(
            SELECT id, url, output_path, status, progress, total_bytes,
                   downloaded_bytes, speed, error_message, options_json,
                   created_at, updated_at, completed_at
            FROM tasks WHERE status IN (?, ?);
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return results;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(TaskStatus::Preparing));
        sqlite3_bind_int(stmt, 2, static_cast<int>(TaskStatus::Downloading));

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(parse_task_record(stmt));
        }

        sqlite3_finalize(stmt);
        return results;
    }

    int count_tasks_by_status(TaskStatus status) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return 0;

        const char* sql = "SELECT COUNT(*) FROM tasks WHERE status = ?;";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return 0;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(status));

        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }

        sqlite3_finalize(stmt);
        return count;
    }

    int count_all_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return 0;

        const char* sql = "SELECT COUNT(*) FROM tasks;";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return 0;
        }

        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }

        sqlite3_finalize(stmt);
        return count;
    }

    bool update_progress(TaskId id, Bytes downloaded_bytes, double progress, BytesPerSecond speed) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const char* sql = R"sql(
            UPDATE tasks SET downloaded_bytes = ?, progress = ?, speed = ?, updated_at = ?
            WHERE id = ?;
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(downloaded_bytes));
        sqlite3_bind_double(stmt, 2, progress);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(speed));
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(id));

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        return sqlite3_changes(db_) > 0;
    }

    bool update_status(TaskId id, TaskStatus status, const std::string& error_message) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const char* sql = R"sql(
            UPDATE tasks SET status = ?, error_message = ?, updated_at = ?
            WHERE id = ?;
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(status));
        sqlite3_bind_text(stmt, 2, error_message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(id));

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        return sqlite3_changes(db_) > 0;
    }

    bool mark_completed(TaskId id) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const char* sql = R"sql(
            UPDATE tasks SET status = ?, progress = 1.0, completed_at = ?, updated_at = ?
            WHERE id = ?;
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(TaskStatus::Completed));
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(id));

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return false;
        }

        return sqlite3_changes(db_) > 0;
    }

    bool mark_failed(TaskId id, const std::string& error_message) {
        return update_status(id, TaskStatus::Failed, error_message);
    }

    int cleanup_completed_tasks(int retention_days) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return 0;

        auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() - std::chrono::hours(24 * retention_days))
                .time_since_epoch()).count();

        const char* sql = R"sql(
            DELETE FROM tasks
            WHERE status = ? AND completed_at > 0 AND completed_at < ?;
        )sql";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return 0;
        }

        sqlite3_bind_int(stmt, 1, static_cast<int>(TaskStatus::Completed));
        sqlite3_bind_int64(stmt, 2, cutoff);

        rc = sqlite3_step(stmt);
        int deleted = sqlite3_changes(db_);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            last_error_ = sqlite3_errmsg(db_);
            return 0;
        }

        return deleted;
    }

    bool delete_all_tasks() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        return exec_sql("DELETE FROM tasks;");
    }

    bool vacuum() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!db_) return false;

        return exec_sql("VACUUM;");
    }

    std::string get_db_path() const {
        return config_.db_path;
    }

    std::string get_last_error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    bool create_tables() {
        if (!exec_sql(CREATE_TASKS_TABLE)) return false;
        if (!exec_sql(CREATE_INDEX_STATUS)) return false;
        if (!exec_sql(CREATE_INDEX_CREATED)) return false;
        return true;
    }

    bool exec_sql(const char* sql) {
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            last_error_ = err_msg ? err_msg : "Unknown error";
            sqlite3_free(err_msg);
            return false;
        }
        return true;
    }

    TaskRecord parse_task_record(sqlite3_stmt* stmt) const {
        TaskRecord record;

        record.id = static_cast<TaskId>(sqlite3_column_int64(stmt, 0));
        record.url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.output_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.status = static_cast<TaskStatus>(sqlite3_column_int(stmt, 3));
        record.progress = sqlite3_column_double(stmt, 4);
        record.total_bytes = static_cast<Bytes>(sqlite3_column_int64(stmt, 5));
        record.downloaded_bytes = static_cast<Bytes>(sqlite3_column_int64(stmt, 6));
        record.speed = static_cast<BytesPerSecond>(sqlite3_column_int64(stmt, 7));

        const char* err = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        if (err) record.error_message = err;

        // Parse options JSON
        const char* opts_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        if (opts_json) {
            try {
                auto json = nlohmann::json::parse(opts_json);
                record.options = deserialize_options(json);
            } catch (const std::exception& e) {
                FALCON_LOG_WARN_STREAM("Failed to parse options JSON: " << e.what());
            }
        }

        auto created_ts = sqlite3_column_int64(stmt, 10);
        auto updated_ts = sqlite3_column_int64(stmt, 11);
        auto completed_ts = sqlite3_column_int64(stmt, 12);

        record.created_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(created_ts));
        record.updated_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(updated_ts));

        if (completed_ts > 0) {
            record.completed_at = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(completed_ts));
        }

        return record;
    }

    nlohmann::json serialize_options(const DownloadOptions& opts) const {
        nlohmann::json j;

        j["max_connections"] = opts.max_connections;
        j["timeout_seconds"] = opts.timeout_seconds;
        j["max_retries"] = opts.max_retries;
        j["retry_delay_seconds"] = opts.retry_delay_seconds;
        j["output_directory"] = opts.output_directory;
        j["output_filename"] = opts.output_filename;
        j["speed_limit"] = opts.speed_limit;
        j["resume_enabled"] = opts.resume_enabled;
        j["user_agent"] = opts.user_agent;
        j["proxy"] = opts.proxy;
        j["proxy_type"] = opts.proxy_type;
        j["proxy_username"] = opts.proxy_username;
        j["proxy_password"] = opts.proxy_password;
        j["verify_ssl"] = opts.verify_ssl;
        j["referer"] = opts.referer;
        j["cookie_file"] = opts.cookie_file;
        j["cookie_jar"] = opts.cookie_jar;
        j["min_segment_size"] = opts.min_segment_size;
        j["adaptive_segment_sizing"] = opts.adaptive_segment_sizing;
        j["progress_interval_ms"] = opts.progress_interval_ms;
        j["create_directory"] = opts.create_directory;
        j["overwrite_existing"] = opts.overwrite_existing;

        if (!opts.headers.empty()) {
            j["headers"] = opts.headers;
        }

        return j;
    }

    DownloadOptions deserialize_options(const nlohmann::json& j) const {
        DownloadOptions opts;

        if (j.contains("max_connections")) opts.max_connections = j["max_connections"];
        if (j.contains("timeout_seconds")) opts.timeout_seconds = j["timeout_seconds"];
        if (j.contains("max_retries")) opts.max_retries = j["max_retries"];
        if (j.contains("retry_delay_seconds")) opts.retry_delay_seconds = j["retry_delay_seconds"];
        if (j.contains("output_directory")) opts.output_directory = j["output_directory"].get<std::string>();
        if (j.contains("output_filename")) opts.output_filename = j["output_filename"].get<std::string>();
        if (j.contains("speed_limit")) opts.speed_limit = j["speed_limit"];
        if (j.contains("resume_enabled")) opts.resume_enabled = j["resume_enabled"];
        if (j.contains("user_agent")) opts.user_agent = j["user_agent"].get<std::string>();
        if (j.contains("proxy")) opts.proxy = j["proxy"].get<std::string>();
        if (j.contains("proxy_type")) opts.proxy_type = j["proxy_type"].get<std::string>();
        if (j.contains("proxy_username")) opts.proxy_username = j["proxy_username"].get<std::string>();
        if (j.contains("proxy_password")) opts.proxy_password = j["proxy_password"].get<std::string>();
        if (j.contains("verify_ssl")) opts.verify_ssl = j["verify_ssl"];
        if (j.contains("referer")) opts.referer = j["referer"].get<std::string>();
        if (j.contains("cookie_file")) opts.cookie_file = j["cookie_file"].get<std::string>();
        if (j.contains("cookie_jar")) opts.cookie_jar = j["cookie_jar"].get<std::string>();
        if (j.contains("min_segment_size")) opts.min_segment_size = j["min_segment_size"];
        if (j.contains("adaptive_segment_sizing")) opts.adaptive_segment_sizing = j["adaptive_segment_sizing"];
        if (j.contains("progress_interval_ms")) opts.progress_interval_ms = j["progress_interval_ms"];
        if (j.contains("create_directory")) opts.create_directory = j["create_directory"];
        if (j.contains("overwrite_existing")) opts.overwrite_existing = j["overwrite_existing"];

        if (j.contains("headers")) {
            for (auto& [k, v] : j["headers"].items()) {
                opts.headers[k] = v.get<std::string>();
            }
        }

        return opts;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    TaskStorageConfig config_;
    sqlite3* db_;
    mutable std::mutex mutex_;
    mutable std::string last_error_;
};

// TaskStorage implementation - forward to Impl

TaskStorage::TaskStorage(const TaskStorageConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

TaskStorage::~TaskStorage() = default;

TaskStorage::TaskStorage(TaskStorage&&) noexcept = default;
TaskStorage& TaskStorage::operator=(TaskStorage&&) noexcept = default;

bool TaskStorage::initialize() { return impl_->initialize(); }
bool TaskStorage::is_ready() const { return impl_->is_ready(); }

TaskId TaskStorage::create_task(const TaskRecord& record) { return impl_->create_task(record); }
std::optional<TaskRecord> TaskStorage::get_task(TaskId id) const { return impl_->get_task(id); }
bool TaskStorage::update_task(TaskId id, const TaskRecord& record) { return impl_->update_task(id, record); }
bool TaskStorage::delete_task(TaskId id) { return impl_->delete_task(id); }
bool TaskStorage::task_exists(TaskId id) const { return impl_->task_exists(id); }

std::vector<TaskRecord> TaskStorage::list_tasks(const TaskQueryOptions& options) const {
    return impl_->list_tasks(options);
}

std::vector<TaskRecord> TaskStorage::get_tasks_by_status(TaskStatus status) const {
    return impl_->get_tasks_by_status(status);
}

std::vector<TaskRecord> TaskStorage::get_active_tasks() const {
    return impl_->get_active_tasks();
}

int TaskStorage::count_tasks_by_status(TaskStatus status) const {
    return impl_->count_tasks_by_status(status);
}

int TaskStorage::count_all_tasks() const { return impl_->count_all_tasks(); }

bool TaskStorage::update_progress(TaskId id, Bytes downloaded_bytes, double progress, BytesPerSecond speed) {
    return impl_->update_progress(id, downloaded_bytes, progress, speed);
}

bool TaskStorage::update_status(TaskId id, TaskStatus status, const std::string& error_message) {
    return impl_->update_status(id, status, error_message);
}

bool TaskStorage::mark_completed(TaskId id) { return impl_->mark_completed(id); }
bool TaskStorage::mark_failed(TaskId id, const std::string& error_message) {
    return impl_->mark_failed(id, error_message);
}

int TaskStorage::cleanup_completed_tasks(int retention_days) {
    return impl_->cleanup_completed_tasks(retention_days);
}

bool TaskStorage::delete_all_tasks() { return impl_->delete_all_tasks(); }
bool TaskStorage::vacuum() { return impl_->vacuum(); }

std::string TaskStorage::get_db_path() const { return impl_->get_db_path(); }
std::string TaskStorage::get_last_error() const { return impl_->get_last_error(); }

} // namespace falcon::daemon
