#include "json_rpc_server.hpp"
#include "json_rpc_server_detail.hpp"

#include <falcon/logger.hpp>

#include <nlohmann/json.hpp>

#ifdef FALCON_HAS_SQLITE3
#include "storage/task_storage.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace falcon::daemon::rpc {

using json = nlohmann::json;

nlohmann::json JsonRpcServer::dispatch_rpc(const std::string& method, nlohmann::json params) {
    // MCP tools/call 翻译层与 JSON-RPC 共用的方法分发（原 handle_jsonrpc 内联
    // lambda 提取）：token 容错剥离后按方法分发；业务错误以
    // {"error":{code,message}} 形状返回，由调用方决定映射为
    // JSON-RPC error 还是 MCP result.isError。
    // For system.multicall inner calls, some clients may redundantly include token again.
    detail::maybe_strip_token(params, auth_secret());

    if (method == "system.listMethods") {
        return json::array({
            "aria2.addUri",
            "aria2.pause",
            "aria2.pauseAll",
            "aria2.forcePause",
            "aria2.unpause",
            "aria2.unpauseAll",
            "aria2.remove",
            "aria2.forceRemove",
            "aria2.forceShutdown",
            "aria2.tellStatus",
            "aria2.tellActive",
            "aria2.tellWaiting",
            "aria2.tellStopped",
            "aria2.getFiles",
            "aria2.getUris",
            "aria2.getOption",
            "aria2.getGlobalOption",
            "aria2.changeGlobalOption",
            "aria2.changePriority",
            "aria2.getGlobalStat",
            "aria2.getVersion",
            "aria2.getSessionInfo",
            "aria2.saveSession",
            "aria2.purgeDownloadResult",
            "aria2.removeDownloadResult",
            "falcon.stopSeeding",
            "falcon.swarm.status",
            "falcon.swarm.setShare",
            "system.listMethods",
            "system.multicall",
        });
    }

    if (method == "system.multicall") {
        if (!params.is_array() || params.empty() || !params[0].is_array()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        json results = json::array();
        for (const auto& call : params[0]) {
            // 非对象或缺失字符串 methodName 的条目不是合法调用（JSON-RPC 2.0）
            if (!call.is_object() || !call.contains("methodName") ||
                !call["methodName"].is_string()) {
                results.push_back(json{{"code", -32600}, {"message", "Invalid Request"}});
                continue;
            }
            const std::string cm = call.value("methodName", "");
            json cp = call.value("params", json::array());
            json r = dispatch_rpc(cm, cp);
            if (r.is_object() && r.contains("error")) {
                results.push_back(r["error"]);
            } else {
                results.push_back(json::array({r}));
            }
        }
        return results;
    }

    if (method == "aria2.getVersion") {
        json out;
        out["version"] = "0.1.0";
        out["enabledFeatures"] = json::array({"jsonrpc", "asyncdns", "https"});
        return out;
    }

    if (method == "aria2.getSessionInfo") {
        return json{{"sessionId", session_id_}};
    }

    // 会话通过 TaskStorage 持续落库，无需显式快照；
    // 接受 aria2 的 filePath 参数但忽略
    if (method == "aria2.saveSession") {
        return "OK";
    }

    if (method == "aria2.pauseAll" || method == "aria2.unpauseAll") {
#ifdef FALCON_HAS_SQLITE3
        std::vector<falcon::TaskId> affected;
        if (storage_) {
            // pause 对 Downloading 任务经 handler 异步生效，回读有竞态；
            // 先收集受影响任务，操作后按 aria2 语义直接落目标状态
            if (method == "aria2.pauseAll") {
                for (const auto& t : engine_->get_active_tasks()) {
                    if (t) affected.push_back(t->id());
                }
            } else {
                for (const auto& t :
                     engine_->get_tasks_by_status(falcon::TaskStatus::Paused)) {
                    if (t) affected.push_back(t->id());
                }
            }
        }
#endif
        if (method == "aria2.pauseAll") {
            engine_->pause_all();
        } else {
            engine_->resume_all();
        }
#ifdef FALCON_HAS_SQLITE3
        if (storage_) {
            if (method == "aria2.pauseAll") {
                for (auto id : affected) {
                    storage_->update_status(id, TaskStatus::Paused);
                }
            } else {
                // 恢复经任务队列重新入队，RPC 返回时通常为 Pending，
                // 后续状态变化由持久化监听器异步修正
                for (auto id : affected) {
                    if (auto t = engine_->get_task(id)) {
                        storage_->update_status(id, t->status());
                    }
                }
            }
        }
#endif
        return "OK";
    }

    if (method == "aria2.forceShutdown" || method == "aria2.shutdown") {
        FALCON_LOG_WARN_STREAM("Daemon shutdown requested via JSON-RPC (" << method << ")");
        if (shutdown_handler_) {
            shutdown_handler_();
        } else {
            FALCON_LOG_WARN_STREAM("No shutdown handler registered; ignoring " << method);
        }
        return "OK";
    }

    if (method == "aria2.purgeDownloadResult") {
        engine_->remove_finished_tasks();
#ifdef FALCON_HAS_SQLITE3
        if (storage_) {
            for (auto status : {falcon::TaskStatus::Completed, falcon::TaskStatus::Failed,
                                falcon::TaskStatus::Cancelled}) {
                for (const auto& record : storage_->get_tasks_by_status(status)) {
                    storage_->delete_task(record.id);
                }
            }
        }
#endif
        return "OK";
    }

    if (method == "aria2.getGlobalOption") {
        json out;
        out["max-overall-download-limit"] = std::to_string(engine_->get_global_speed_limit());
        out["max-concurrent-downloads"] = std::to_string(engine_->get_max_concurrent_tasks());
        // daemon 没有全局默认下载目录的概念（addUri 逐任务指定），保持 aria2 键位存在
        out["dir"] = "";
        return out;
    }

    if (method == "aria2.changePriority") {
        // params = [gid, priority]；priority 用 falcon::TaskPriority 的数值
        // （0=Low 1=Normal 2=High 3=Critical），与 aria2 原生语义不同，
        // 这是 Falcon 客户端约定
        if (!params.is_array() || params.size() < 2 || !params[0].is_string() ||
            (!params[1].is_number_integer() && !params[1].is_string())) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        auto tid = detail::gid_to_task_id(params[0].get<std::string>());
        if (!tid) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        int value = -1;
        try {
            value = params[1].is_number_integer()
                        ? params[1].get<int>()
                        : std::stoi(params[1].get<std::string>());
        } catch (const std::exception&) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid priority"}}}};
        }
        if (value < 0 || value > 3) {
            return json{{"error", json{{"code", 1},
                                       {"message", "Priority must be 0..3"}}}};
        }
        auto task = engine_->get_task(*tid);
        if (!task) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        engine_->adjust_task_priority(
            *tid, static_cast<falcon::TaskPriority>(value));
        return detail::task_id_to_gid(*tid);
    }

    if (method == "falcon.stopSeeding") {
        // Falcon 扩展：params = [gid]——请求任务停止做种（BitTorrent）。
        // 置粘性标志，BT 监控循环下一 tick 与策略达标同路径收口。
        // 错误码约定：1 = 任务不支持做种（非 BT/未在做种），2 = gid 不存在
        if (!params.is_array() || params.empty() || !params[0].is_string()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        auto tid = detail::gid_to_task_id(params[0].get<std::string>());
        if (!tid) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        auto task = engine_->get_task(*tid);
        if (!task) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        const auto seed = task->get_seed_info();
        if (!seed.seeding_active) {
            return json{{"error", json{{"code", 1},
                                       {"message", "Task is not seeding"}}}};
        }
        task->request_stop_seeding();
        return detail::task_id_to_gid(*tid);
    }

    if (method == "falcon.swarm.status") {
        // Falcon 扩展（§16.5）：announcer 快照。处理器由 main 注入——
        // FALCON_HAS_SWARM 缺席（未链 swarm client）时未注册，报 -32603
        if (!swarm_status_handler_) {
            return json{{"error", json{{"code", -32603},
                                       {"message", "Swarm announcer unavailable"}}}};
        }
        return swarm_status_handler_();
    }

    if (method == "falcon.swarm.setShare") {
        // Falcon 扩展（§16.5）：params = [{enabled: bool}]——运行期共享
        // 开关（与 SIGHUP 热更同语义：翻关全量 retract，announcer 不停机）
        if (!swarm_share_handler_) {
            return json{{"error", json{{"code", -32603},
                                       {"message", "Swarm announcer unavailable"}}}};
        }
        if (!params.is_array() || params.empty() || !params[0].is_object() ||
            !params[0].contains("enabled") || !params[0]["enabled"].is_boolean()) {
            return json{{"error", json{{"code", -32602},
                                       {"message", "Invalid params"}}}};
        }
        return swarm_share_handler_(params[0]["enabled"].get<bool>());
    }

    if (method == "aria2.changeGlobalOption") {
        // aria2 签名是 (secret, options)：token 剥离后 options 就在 params[0]
        if (!params.is_array() || params.empty() || !params[0].is_object()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        json result;
        for (const auto& [key, value] : params[0].items()) {
            try {
                if (key == "max-overall-download-limit") {
                    const std::string v = value.is_string()
                                              ? value.get<std::string>()
                                              : value.dump();
                    falcon::BytesPerSecond limit = 0;
                    if (v != "none" && v != "0") {
                        limit = static_cast<falcon::BytesPerSecond>(std::stoull(v));
                    }
                    engine_->set_global_speed_limit(limit);
                } else if (key == "max-concurrent-downloads") {
                    const std::size_t n = value.is_number_integer()
                        ? static_cast<std::size_t>(value.get<int>())
                        : static_cast<std::size_t>(std::stoull(value.get<std::string>()));
                    engine_->set_max_concurrent_tasks(n);
                } else {
                    return json{{"error", json{{"code", 1},
                                               {"message", "Option not supported: " + key}}}};
                }
            } catch (const std::exception&) {
                return json{{"error", json{{"code", 1},
                                           {"message", "Invalid option value: " + key}}}};
            }
        }
        return "OK";
    }

    if (method == "aria2.getGlobalStat") {
        auto tasks = engine_->get_all_tasks();
        std::size_t active = 0, waiting = 0, stopped = 0;
        for (const auto& t : tasks) {
            if (!t) continue;
            switch (t->status()) {
                case falcon::TaskStatus::Downloading:
                case falcon::TaskStatus::Preparing:
                    active++;
                    break;
                case falcon::TaskStatus::Pending:
                case falcon::TaskStatus::Paused:
                    waiting++;
                    break;
                case falcon::TaskStatus::Completed:
                case falcon::TaskStatus::Failed:
                case falcon::TaskStatus::Cancelled:
                    stopped++;
                    break;
            }
        }

#ifdef FALCON_HAS_SQLITE3
        // 重启后终态历史只存在于 storage（引擎恢复时跳过终态），
        // 计数需合并 storage 独有记录，与 tell* 视图保持一致
        if (storage_) {
            waiting += detail::storage_extra_records(
                storage_, tasks, {falcon::TaskStatus::Pending, falcon::TaskStatus::Paused}).size();
            stopped += detail::storage_extra_records(
                storage_, tasks,
                {falcon::TaskStatus::Completed, falcon::TaskStatus::Failed,
                 falcon::TaskStatus::Cancelled}).size();
        }
#endif

        json out;
        out["downloadSpeed"] = std::to_string(engine_->get_total_speed());
        out["uploadSpeed"] = "0";
        out["numActive"] = std::to_string(active);
        out["numWaiting"] = std::to_string(waiting);
        out["numStopped"] = std::to_string(stopped);
        out["numStoppedTotal"] = std::to_string(stopped);
        return out;
    }

    if (method == "aria2.addUri") {
        if (!params.is_array() || params.empty() || !params[0].is_array() || params[0].empty()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        const auto& uris = params[0];
        const std::string url = uris[0].get<std::string>();

        falcon::DownloadOptions options;
        if (params.size() >= 2 && params[1].is_object()) {
            const auto& o = params[1];
            if (o.contains("dir") && o["dir"].is_string()) options.output_directory = o["dir"].get<std::string>();
            if (o.contains("out") && o["out"].is_string()) options.output_filename = o["out"].get<std::string>();
            if (o.contains("user-agent") && o["user-agent"].is_string()) options.user_agent = o["user-agent"].get<std::string>();
            if (o.contains("referer") && o["referer"].is_string()) options.referer = o["referer"].get<std::string>();
            if (o.contains("load-cookies") && o["load-cookies"].is_string()) options.cookie_file = o["load-cookies"].get<std::string>();
            if (o.contains("save-cookies") && o["save-cookies"].is_string()) options.cookie_jar = o["save-cookies"].get<std::string>();
            if (o.contains("http-user") && o["http-user"].is_string()) options.http_username = o["http-user"].get<std::string>();
            if (o.contains("http-passwd") && o["http-passwd"].is_string()) options.http_password = o["http-passwd"].get<std::string>();
            if (o.contains("certificate") && o["certificate"].is_string()) options.client_certificate = o["certificate"].get<std::string>();
            if (o.contains("private-key") && o["private-key"].is_string()) options.client_private_key = o["private-key"].get<std::string>();
            if (o.contains("all-proxy") && o["all-proxy"].is_string()) options.proxy = o["all-proxy"].get<std::string>();
            if (o.contains("all-proxy-user") && o["all-proxy-user"].is_string()) options.proxy_username = o["all-proxy-user"].get<std::string>();
            if (o.contains("all-proxy-passwd") && o["all-proxy-passwd"].is_string()) options.proxy_password = o["all-proxy-passwd"].get<std::string>();

            if (o.contains("check-certificate")) {
                auto v = detail::parse_bool_option(o["check-certificate"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: check-certificate"}}}};
                options.verify_ssl = *v;
            }

            if (o.contains("max-tries")) {
                auto v = detail::parse_numeric_option(o["max-tries"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: max-tries"}}}};
                options.max_retries = *v;
            }
            if (o.contains("retry-wait")) {
                auto v = detail::parse_numeric_option(o["retry-wait"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: retry-wait"}}}};
                options.retry_delay_seconds = *v;
            }
            if (o.contains("timeout")) {
                auto v = detail::parse_numeric_option(o["timeout"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: timeout"}}}};
                options.timeout_seconds = *v;
            }
            if (o.contains("max-connection-per-server")) {
                auto v = detail::parse_numeric_option(o["max-connection-per-server"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: max-connection-per-server"}}}};
                options.max_connections = *v;
            }
            // min-split-size（aria2 同名键）：分段最小段长，接受裸字
            // 节数与 K/M/G/T 后缀形态（复用核心 parse_size_with_suffix，
            // 永不抛，溢出即 -32602）
            if (o.contains("min-split-size")) {
                if (!o["min-split-size"].is_string()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: min-split-size"}}}};
                }
                auto v = falcon::parse_size_with_suffix(o["min-split-size"].get<std::string>());
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: min-split-size"}}}};
                options.min_segment_size = static_cast<std::size_t>(*v);
            }
            if (o.contains("max-download-limit")) {
                auto v = detail::parse_numeric_option(o["max-download-limit"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: max-download-limit"}}}};
                options.speed_limit = static_cast<falcon::BytesPerSecond>(*v);
            }

            // split / continue / allow-overwrite / auto-file-renaming：
            // MCP falcon_add_download 白名单与文档长期广告，补齐到
            // V1 DownloadOptions 的真实字段（分段连接数 / 续传 /
            // 覆盖 / 自动改名），不再静默丢弃
            if (o.contains("split")) {
                auto v = detail::parse_numeric_option(o["split"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: split"}}}};
                options.max_connections = *v;
            }
            if (o.contains("continue")) {
                auto v = detail::parse_bool_option(o["continue"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: continue"}}}};
                options.resume_enabled = *v;
            }
            if (o.contains("allow-overwrite")) {
                auto v = detail::parse_bool_option(o["allow-overwrite"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: allow-overwrite"}}}};
                options.overwrite_existing = *v;
            }
            if (o.contains("auto-file-renaming")) {
                auto v = detail::parse_bool_option(o["auto-file-renaming"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: auto-file-renaming"}}}};
                options.auto_file_renaming = *v;
            }

            // aria2 客户端兼容字段；单一 V1 引擎当前不消费，仅透传到
            // DownloadOptions 并随任务持久化，不再声称有引擎侧门禁
            if (o.contains("file-allocation")) {
                if (!o["file-allocation"].is_string()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: file-allocation"}}}};
                }
                options.file_allocation = o["file-allocation"].get<std::string>();
            }
            if (o.contains("conditional-get")) {
                auto v = detail::parse_bool_option(o["conditional-get"]);
                if (!v) return json{{"error", json{{"code", -32602}, {"message", "Invalid value for option: conditional-get"}}}};
                options.conditional_get = *v;
            }

            // 做种策略（BitTorrent）：字符串（aria2 风格）与数值双
            // 形态；负值钳 0（与 CLI --seed-ratio/--seed-time 同语义）
            if (o.contains("seed-ratio")) {
                double ratio = -1.0;
                if (o["seed-ratio"].is_string()) {
                    try {
                        ratio = std::stod(o["seed-ratio"].get<std::string>());
                    } catch (const std::exception&) {
                        ratio = -1.0;
                    }
                }
                if (o["seed-ratio"].is_number()) {
                    ratio = o["seed-ratio"].get<double>();
                }
                options.seed_ratio = std::max(0.0, ratio);
            }
            if (o.contains("seed-time")) {
                double minutes = -1.0;
                if (o["seed-time"].is_string()) {
                    try {
                        minutes = std::stod(o["seed-time"].get<std::string>());
                    } catch (const std::exception&) {
                        minutes = -1.0;
                    }
                }
                if (o["seed-time"].is_number()) {
                    minutes = o["seed-time"].get<double>();
                }
                options.seed_time_minutes = static_cast<std::size_t>(
                    std::max(0.0, minutes));
            }

            // P2SP 共享三态（§16.5）：仅接受 "true"/"false" 字符串
            //（aria2 风格）；缺省 = ""（回落全局开关），其余值 -32602
            if (o.contains("p2sp-share")) {
                if (!o["p2sp-share"].is_string() ||
                    (o["p2sp-share"].get<std::string>() != "true" &&
                     o["p2sp-share"].get<std::string>() != "false")) {
                    return json{{"error", json{{"code", -32602},
                                               {"message", "p2sp-share must be \"true\" or \"false\""}}}};
                }
                options.p2sp_share = o["p2sp-share"].get<std::string>();
            }

            if (o.contains("header")) {
                if (o["header"].is_array()) {
                    for (const auto& hv : o["header"]) {
                        if (!hv.is_string()) continue;
                        const std::string h = hv.get<std::string>();
                        auto pos = h.find(':');
                        if (pos == std::string::npos) continue;
                        options.headers[detail::trim(h.substr(0, pos))] = detail::trim(h.substr(pos + 1));
                    }
                } else if (o["header"].is_string()) {
                    const std::string h = o["header"].get<std::string>();
                    auto pos = h.find(':');
                    if (pos != std::string::npos) {
                        options.headers[detail::trim(h.substr(0, pos))] = detail::trim(h.substr(pos + 1));
                    }
                }
            }
        }

        auto task = engine_->add_task(url, options);
        if (!task) {
            return json{{"error", json{{"code", 1}, {"message", "Unsupported URL"}}}};
        }

#ifdef FALCON_HAS_SQLITE3
        // Persist task to database if storage is available
        if (storage_) {
            TaskRecord record;
            record.id = task->id();
            record.url = task->url();
            record.output_path = task->output_path();
            record.status = task->status();
            record.progress = task->progress();
            record.total_bytes = task->total_bytes();
            record.downloaded_bytes = task->downloaded_bytes();
            record.speed = task->speed();
            record.error_message = task->error_message();
            record.options = options;
            record.created_at = std::chrono::system_clock::now();
            record.updated_at = std::chrono::system_clock::now();

            storage_->create_task(record);
        }
#endif

        engine_->start_task(task->id());
        return detail::task_id_to_gid(task->id());
    }

    if (method == "aria2.tellStatus") {
        if (!params.is_array() || params.empty() || !params[0].is_string()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        auto tid = detail::gid_to_task_id(params[0].get<std::string>());
        if (!tid) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        // 引擎内存态优先；引擎查不到时（重启后终态历史）回落到 storage
        if (auto task = engine_->get_task(*tid)) {
            return detail::task_to_status_json(*task);
        }
#ifdef FALCON_HAS_SQLITE3
        if (storage_) {
            if (auto record = storage_->get_task(*tid)) {
                return detail::task_record_to_status_json(*record);
            }
        }
#endif
        return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
    }

    if (method == "aria2.getFiles" || method == "aria2.getUris" || method == "aria2.getOption") {
        if (!params.is_array() || params.empty() || !params[0].is_string()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        auto tid = detail::gid_to_task_id(params[0].get<std::string>());
        if (!tid) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }

        auto task = engine_->get_task(*tid);
#ifdef FALCON_HAS_SQLITE3
        std::optional<TaskRecord> record;
        if (!task && storage_) {
            record = storage_->get_task(*tid);
        }
        if (!task && !record) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        if (method == "aria2.getFiles") {
            if (task) {
                return detail::files_json(task->output_path(), task->total_bytes(),
                                  task->downloaded_bytes(), task->url(), true);
            }
            return detail::files_json(record->output_path, record->total_bytes,
                              record->downloaded_bytes, record->url, true);
        }
        if (method == "aria2.getUris") {
            const std::string& uri = task ? task->url() : record->url;
            return json::array({json{{"uri", uri}, {"status", "used"}}});
        }
        return detail::download_options_to_json(task ? task->options() : record->options);
#else
        if (!task) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        if (method == "aria2.getFiles") {
            return detail::files_json(task->output_path(), task->total_bytes(),
                              task->downloaded_bytes(), task->url(), true);
        }
        if (method == "aria2.getUris") {
            return json::array({json{{"uri", task->url()}, {"status", "used"}}});
        }
        return detail::download_options_to_json(task->options());
#endif
    }

    if (method == "aria2.pause" || method == "aria2.forcePause" ||
        method == "aria2.unpause" ||
        method == "aria2.remove" || method == "aria2.forceRemove") {
        if (!params.is_array() || params.empty() || !params[0].is_string()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        const std::string gid = params[0].get<std::string>();
        auto tid = detail::gid_to_task_id(gid);
        if (!tid) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        auto task = engine_->get_task(*tid);
        if (!task) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }

        if (method == "aria2.pause" || method == "aria2.forcePause") {
            if (!engine_->pause_task(*tid)) {
                return json{{"error", json{{"code", 1}, {"message", "Pause failed"}}}};
            }
#ifdef FALCON_HAS_SQLITE3
            // Update storage
            if (storage_) storage_->update_status(*tid, TaskStatus::Paused);
#endif
            return gid;
        }
        if (method == "aria2.unpause") {
            if (!engine_->resume_task(*tid)) {
                return json{{"error", json{{"code", 1}, {"message", "Resume failed"}}}};
            }
#ifdef FALCON_HAS_SQLITE3
            // Update storage - task will be in Downloading state
            if (storage_) {
                auto current_task = engine_->get_task(*tid);
                if (current_task) {
                    storage_->update_status(*tid, current_task->status());
                }
            }
#endif
            return gid;
        }
        // aria2.remove / aria2.forceRemove
        if (!engine_->cancel_task(*tid)) {
            return json{{"error", json{{"code", 1}, {"message", "Remove failed"}}}};
        }
#ifdef FALCON_HAS_SQLITE3
        // Update storage
        if (storage_) storage_->update_status(*tid, TaskStatus::Cancelled);
#endif
        return gid;
    }

    if (method == "aria2.tellActive") {
        json out = json::array();
        for (const auto& t : engine_->get_active_tasks()) {
            if (!t) continue;
            out.push_back(detail::task_to_status_json(*t));
        }
        return out;
    }

    if (method == "aria2.tellWaiting") {
        if (!params.is_array() || params.size() < 2) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        const int offset = params[0].is_number_integer() ? params[0].get<int>() : std::stoi(params[0].get<std::string>());
        const int num = params[1].is_number_integer() ? params[1].get<int>() : std::stoi(params[1].get<std::string>());

        std::vector<json> items;
        auto tasks = engine_->get_all_tasks();
        for (const auto& t : tasks) {
            if (!t) continue;
            if (t->status() == falcon::TaskStatus::Pending ||
                t->status() == falcon::TaskStatus::Paused) {
                items.push_back(detail::task_to_status_json(*t));
            }
        }
#ifdef FALCON_HAS_SQLITE3
        if (storage_) {
            for (const auto& record : detail::storage_extra_records(
                     storage_, tasks,
                     {falcon::TaskStatus::Pending, falcon::TaskStatus::Paused})) {
                items.push_back(detail::task_record_to_status_json(record));
            }
        }
#endif
        return detail::paginate_items(items, offset, num);
    }

    if (method == "aria2.tellStopped") {
        if (!params.is_array() || params.size() < 2) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        const int offset = params[0].is_number_integer() ? params[0].get<int>() : std::stoi(params[0].get<std::string>());
        const int num = params[1].is_number_integer() ? params[1].get<int>() : std::stoi(params[1].get<std::string>());

        std::vector<json> items;
        auto tasks = engine_->get_all_tasks();
        for (const auto& t : tasks) {
            if (!t) continue;
            if (t->status() == falcon::TaskStatus::Completed ||
                t->status() == falcon::TaskStatus::Failed ||
                t->status() == falcon::TaskStatus::Cancelled) {
                items.push_back(detail::task_to_status_json(*t));
            }
        }
#ifdef FALCON_HAS_SQLITE3
        if (storage_) {
            for (const auto& record : detail::storage_extra_records(
                     storage_, tasks,
                     {falcon::TaskStatus::Completed, falcon::TaskStatus::Failed,
                      falcon::TaskStatus::Cancelled})) {
                items.push_back(detail::task_record_to_status_json(record));
            }
        }
#endif
        return detail::paginate_items(items, offset, num);
    }

    if (method == "aria2.removeDownloadResult") {
        if (!params.is_array() || params.empty() || !params[0].is_string()) {
            return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
        }
        auto tid = detail::gid_to_task_id(params[0].get<std::string>());
        if (!tid) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }

        bool existed = false;
        if (auto task = engine_->get_task(*tid)) {
            existed = true;
            // 引擎只允许移除终态任务；活动任务返回失败
            if (!engine_->remove_task(*tid)) {
                return json{{"error", json{{"code", 1},
                                           {"message", "Task cannot be removed while active"}}}};
            }
        }
#ifdef FALCON_HAS_SQLITE3
        if (storage_) {
            if (storage_->task_exists(*tid)) {
                existed = true;
                storage_->delete_task(*tid);
            }
        }
#endif
        if (!existed) {
            return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
        }
        return "OK";
    }

    return json{{"error", json{{"code", -32601}, {"message", "Method not found"}}}};
}

} // namespace falcon::daemon::rpc
