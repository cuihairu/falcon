#pragma once

#include <falcon/download_task.hpp>

#include <memory>
#include <vector>

int generate_summary(const std::vector<std::shared_ptr<falcon::DownloadTask>>& tasks);
