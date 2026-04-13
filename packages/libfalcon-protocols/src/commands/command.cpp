/**
 * @file command.cpp
 * @brief 命令系统公共实现
 * @author Falcon Team
 * @date 2025-12-31
 */

#include <falcon/commands/command.hpp>
#include <falcon/download_engine_v2.hpp>

namespace falcon {

void AbstractCommand::schedule_next(DownloadEngineV2* engine,
                                   std::unique_ptr<Command> next_cmd) {
    if (!engine || !next_cmd) {
        return;
    }
    engine->add_command(std::move(next_cmd));
}

} // namespace falcon

