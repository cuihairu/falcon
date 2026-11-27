/**
 * @file task_manager.cpp
 * @brief 任务管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */
#include "task_manager_impl.hpp"

namespace falcon {

TaskId TaskManager::add_task(DownloadTask::Ptr task, TaskPriority priority) {
    return impl_->add_task(std::move(task), priority);
}

DownloadTask::Ptr TaskManager::get_task(TaskId id) const {
    return impl_->get_task(id);
}

std::vector<DownloadTask::Ptr> TaskManager::get_all_tasks() const {
    return impl_->get_all_tasks();
}

std::vector<DownloadTask::Ptr> TaskManager::get_tasks_by_status(TaskStatus status) const {
    return impl_->get_tasks_by_status(status);
}

std::vector<DownloadTask::Ptr> TaskManager::get_active_tasks() const {
    return impl_->get_active_tasks();
}

bool TaskManager::remove_task(TaskId id) {
    return impl_->remove_task(id);
}

size_t TaskManager::cleanup_finished_tasks() {
    return impl_->cleanup_finished_tasks();
}

void TaskManager::set_cleanup_interval(std::chrono::seconds interval) {
    impl_->set_cleanup_interval(interval);
}

bool TaskManager::pause_task(TaskId id) {
    return impl_->pause_task(id);
}

bool TaskManager::resume_task(TaskId id) {
    return impl_->resume_task(id);
}

bool TaskManager::cancel_task(TaskId id) {
    return impl_->cancel_task(id);
}

void TaskManager::pause_all() {
    impl_->pause_all();
}

void TaskManager::resume_all() {
    impl_->resume_all();
}

void TaskManager::cancel_all() {
    impl_->cancel_all();
}

bool TaskManager::wait_all(std::chrono::milliseconds timeout) {
    return impl_->wait_all(timeout);
}

size_t TaskManager::get_queue_size() const {
    return impl_->get_queue_size();
}

size_t TaskManager::get_active_task_count() const {
    return impl_->get_active_task_count();
}

size_t TaskManager::get_max_concurrent_tasks() const {
    return impl_->get_max_concurrent_tasks();
}

void TaskManager::set_max_concurrent_tasks(size_t max_tasks) {
    impl_->set_max_concurrent_tasks(max_tasks);
}

void TaskManager::set_global_speed_source(std::atomic<std::uint64_t>* source) {
    impl_->set_global_speed_source(source);
}

bool TaskManager::adjust_task_priority(TaskId id, TaskPriority priority) {
    return impl_->adjust_task_priority(id, priority);
}

TaskManager::Statistics TaskManager::get_statistics() const {
    return impl_->get_statistics();
}

bool TaskManager::save_state(const std::string& file_path) {
    return impl_->save_state(file_path);
}

bool TaskManager::load_state(const std::string& file_path) {
    return impl_->load_state(file_path);
}

void TaskManager::set_state_file(const std::string& file_path) {
    impl_->set_state_file(file_path);
}

void TaskManager::start() {
    impl_->start();
}

void TaskManager::stop() {
    impl_->stop();
}

bool TaskManager::is_running() const {
    return impl_->is_running();
}

void TaskManager::on_task_status_changed(TaskId task_id,
                                         TaskStatus old_status,
                                         TaskStatus new_status) {
    impl_->on_status_changed(task_id, old_status, new_status);
}

void TaskManager::on_task_progress(TaskId /*task_id*/, const ProgressInfo& progress) {
    impl_->on_progress(progress);
}

bool TaskManager::start_task(TaskId id) {
    return impl_->start_task(id);
}

bool TaskManager::start_task(TaskId id, TaskPriority priority) {
    return impl_->start_task(id, priority);
}


} // namespace falcon
