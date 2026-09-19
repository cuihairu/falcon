/**
 * @file metalink_handler.cpp
 * @brief Metalink 协议处理器实现
 * @author Falcon Team
 * @date 2026-09-18
 */

#include "metalink_handler.hpp"

#include "mini_xml_parser.hpp"

#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>
#include <falcon/protocol_registry.hpp>
#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/request_group.hpp>
#include <falcon/protocols/v2_engine_host.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

#if defined(FALCON_USE_OPENSSL) || defined(FALCON_HAS_OPENSSL)
#define FALCON_METALINK_HAS_HASH 1
#endif

namespace fs = std::filesystem;

namespace falcon::protocols::metalink {

namespace {

//------------------------------------------------------------------------------
// 小工具
//------------------------------------------------------------------------------

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    const std::size_t len = std::strlen(suffix);
    if (s.size() < len) return false;
    return std::equal(s.end() - static_cast<std::ptrdiff_t>(len), s.end(),
                      suffix, [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

/// 剥掉 query/fragment,返回纯路径部分
std::string strip_query(const std::string& url) {
    return url.substr(0, url.find_first_of("?#"));
}

/// 提取小写 scheme;无 "://" 返回空串
std::string scheme_of(const std::string& url) {
    const auto pos = url.find("://");
    if (pos == std::string::npos) return "";
    return to_lower(url.substr(0, pos));
}

bool is_http_ftp_scheme(const std::string& scheme) {
    return scheme == "http" || scheme == "https" || scheme == "ftp" ||
           scheme == "ftps";
}

/// file:// 或裸本地路径统一转成可打开的路径
bool local_path_from_url(const std::string& url, std::string& out) {
    const std::string scheme = scheme_of(url);
    if (scheme == "file") {
        // file:///path/x.meta4 → /path/x.meta4(file://localhost/ 少见,宽容处理)
        std::string path = url.substr(7);
        if (path.rfind("localhost/", 0) == 0) path = path.substr(10);
        out = path;
        return true;
    }
    if (scheme.empty()) {
        out = url;
        return true;
    }
    return false;
}

/// 拒绝路径穿越与路径分隔符(name 是文件名,不是路径)
bool is_safe_filename(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

/// 属性整数解析;失败/超范围回落默认值
int parse_int_attr(const std::string& text, int fallback) {
    try {
        std::size_t consumed = 0;
        const long value = std::stol(trim(text), &consumed);
        if (consumed != trim(text).size()) return fallback;
        if (value < std::numeric_limits<int>::min() ||
            value > std::numeric_limits<int>::max()) {
            return fallback;
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::uint64_t parse_u64_attr(const std::string& text) {
    try {
        return std::stoull(trim(text));
    } catch (const std::exception&) {
        return 0;
    }
}

/// 镜像排序键:meta4 priority 升序优先,metalink3 preference 降序换算
int mirror_rank(const MetalinkUrl& u) {
    if (u.priority != kNoPriority) return u.priority;
    if (u.preference >= 0) return 1000000 - u.preference;
    return kNoPriority;
}

void sort_urls(std::vector<MetalinkUrl>& urls) {
    std::stable_sort(urls.begin(), urls.end(),
                     [](const MetalinkUrl& a, const MetalinkUrl& b) {
                         return mirror_rank(a) < mirror_rank(b);
                     });
}

std::string join_errors(const std::vector<std::string>& errors) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i) oss << "; ";
        oss << errors[i];
    }
    return oss.str();
}

} // namespace

//==============================================================================
// MetalinkFileParser
//==============================================================================

bool MetalinkFileParser::hash_type_to_algorithm(const std::string& type,
                                                HashAlgorithm& out) {
    const std::string t = to_lower(trim(type));
    if (t == "md5") { out = HashAlgorithm::MD5; return true; }
    if (t == "sha-1" || t == "sha1") { out = HashAlgorithm::SHA1; return true; }
    if (t == "sha-256" || t == "sha256") { out = HashAlgorithm::SHA256; return true; }
    if (t == "sha-512" || t == "sha512") { out = HashAlgorithm::SHA512; return true; }
    return false;
}

std::vector<MetalinkFile> MetalinkFileParser::parse(
    const std::string& xml_text) {
    auto root = MiniXmlParser::parse(xml_text);
    if (!root || root->name != "metalink") {
        throw std::runtime_error(
            "不是有效的 metalink 文档(根元素缺失或不是 metalink)");
    }

    std::vector<MetalinkFile> files;
    for (const auto* file_node : root->children_of("file")) {
        MetalinkFile mf;

        const std::string* name = file_node->attr("name");
        if (!name || name->empty()) {
            throw std::runtime_error("file 元素缺少 name 属性");
        }
        if (!is_safe_filename(*name)) {
            throw std::runtime_error("file name 含路径穿越或分隔符: " + *name);
        }
        mf.name = *name;

        if (const auto* size_node = file_node->child("size")) {
            mf.size = parse_u64_attr(size_node->text);
        }

        for (const auto* hash_node : file_node->children_of("hash")) {
            const std::string* type = hash_node->attr("type");
            HashAlgorithm algo;
            if (!type || !hash_type_to_algorithm(*type, algo)) continue;
            std::string hex = trim(hash_node->text);
            if (hex.empty()) continue;
            mf.hashes.emplace_back(std::move(hex), algo);
        }

        for (const auto* url_node : file_node->children_of("url")) {
            MetalinkUrl mu;
            mu.url = trim(url_node->text);
            if (mu.url.empty()) continue;
            if (const auto* t = url_node->attr("type")) mu.type = *t;
            if (const auto* loc = url_node->attr("location")) {
                mu.location = *loc;
            }
            if (const auto* p = url_node->attr("priority")) {
                mu.priority = parse_int_attr(*p, kNoPriority);
            }
            if (const auto* p = url_node->attr("preference")) {
                mu.preference = parse_int_attr(*p, -1);
            }

            // 只保留委托模式下可下载的镜像:http/ftp;type 显式标注为
            // 其他协议(bittorrent 等)的条目跳过
            const std::string scheme = scheme_of(mu.url);
            if (!is_http_ftp_scheme(scheme)) continue;
            if (!mu.type.empty() && !is_http_ftp_scheme(to_lower(mu.type))) {
                continue;
            }
            mf.urls.push_back(std::move(mu));
        }

        sort_urls(mf.urls);

        if (mf.urls.empty()) {
            throw std::runtime_error("metalink 无可用 HTTP/FTP 镜像: " +
                                     mf.name);
        }
        files.push_back(std::move(mf));
    }

    if (files.empty()) {
        throw std::runtime_error("metalink 文档不含 file 元素");
    }
    return files;
}

//==============================================================================
// MetalinkHandler
//==============================================================================

MetalinkHandler::MetalinkHandler() = default;
MetalinkHandler::~MetalinkHandler() = default;

MetalinkHandler::ActiveContext* MetalinkHandler::find_context(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = contexts_.find(id);
    return it == contexts_.end() ? nullptr : it->second.get();
}

std::vector<std::string> MetalinkHandler::supported_schemes() const {
    return {"meta4", "metalink"};
}

bool MetalinkHandler::can_handle(const std::string& url) const {
    const std::string path = strip_query(url);
    return ends_with_ci(path, ".meta4") || ends_with_ci(path, ".metalink");
}

//------------------------------------------------------------------------------
// metalink 文档获取(本地读文件 / 远程经 HTTP handler 抓取)
//------------------------------------------------------------------------------

namespace {

/// 读整个文件为字符串;失败抛 runtime_error
std::string read_file_content(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("无法打开 metalink 文件: " + path);
    std::ostringstream oss;
    oss << file.rdbuf();
    if (oss.fail()) {
        throw std::runtime_error("读取 metalink 文件失败: " + path);
    }
    return oss.str();
}

} // namespace

std::string MetalinkHandler::fetch_metalink_document(
    const std::string& url, const DownloadOptions& options, TaskId task_id,
    IEventListener* listener, ActiveContext* context) {
    std::string local_path;
    if (local_path_from_url(url, local_path)) {
        return read_file_content(local_path);
    }

    // 远程:必须经 HTTP handler 抓取(不用 get_handler_for_url——
    // 路由特判会指回 metalink 自己造成递归)
    const std::string scheme = scheme_of(url);
    if (scheme != "http" && scheme != "https") {
        throw std::runtime_error("不支持的 metalink 获取方式: " + url);
    }
    IProtocolHandler* http = registry_
                                 ? registry_->get_handler("http")
                                 : nullptr;
    if (!http) {
        throw std::runtime_error(
            "HTTP handler 未注册,无法抓取远程 metalink");
    }

    // 临时文件:meta4 文档小,放临时目录即可;URL 哈希避免同 URL 并发互踩
    const std::size_t url_hash =
        static_cast<std::size_t>(std::hash<std::string>{}(url));
    fs::path tmp = fs::temp_directory_path() /
                   (".falcon-meta4-" + std::to_string(task_id) + "-" +
                    std::to_string(url_hash) + ".tmp");

    DownloadOptions fetch_options = options;
    fetch_options.output_filename.clear();

    auto shadow = std::make_shared<DownloadTask>(task_id, url, fetch_options);
    shadow->set_output_path(tmp.string());
    auto firewall = std::make_shared<MetalinkDelegateListener>(task_id,
                                                                listener);

    // 抓取阶段的影子也注册进上下文,让 pause/cancel 转发能中止数据流
    if (context) {
        std::lock_guard<std::mutex> lock(context->shadow_mutex);
        context->shadow = shadow;
        context->active_target = http;
    }
    shadow->set_listener(firewall.get());
    http->download(shadow, firewall.get());
    if (context) {
        std::lock_guard<std::mutex> lock(context->shadow_mutex);
        context->shadow.reset();
        context->active_target = nullptr;
    }

    if (shadow->status() != TaskStatus::Completed) {
        const std::string detail =
            shadow->error_message().empty() ? "未知错误"
                                            : shadow->error_message();
        throw std::runtime_error("抓取远程 metalink 失败: " + detail);
    }

    std::string content = read_file_content(tmp.string());
    std::error_code ec;
    fs::remove(tmp, ec);
    return content;
}

FileInfo MetalinkHandler::get_file_info(const std::string& url,
                                        const DownloadOptions& options) {
    const std::string xml =
        fetch_metalink_document(url, options, 0, nullptr, nullptr);
    const auto files = MetalinkFileParser::parse(xml);

    FileInfo info;
    info.url = url;
    info.filename = files.front().name;
    info.total_size = files.front().size;
    info.supports_resume = true;
    return info;
}

//------------------------------------------------------------------------------
// 委托数据面
//------------------------------------------------------------------------------

namespace {

/// 把 part 文件按 mf.hashes 逐一校验;通过返回 true,失败写 errors 并删文件
bool verify_or_discard(const MetalinkFile& mf, const std::string& part_path,
                       std::vector<std::string>& errors) {
    if (mf.hashes.empty()) {
        FALCON_LOG_WARN_STREAM(
            "[metalink] " << mf.name << " 无整文件哈希,跳过校验");
        return true;
    }

#ifdef FALCON_METALINK_HAS_HASH
    std::error_code ec;
    if (!fs::exists(part_path, ec)) {
        errors.push_back("下载产物缺失");
        return false;
    }
    for (const auto& [expected, algo] : mf.hashes) {
        const auto result =
            FileHasher::verify_streaming(part_path, expected, algo);
        if (result.valid) return true;
    }
    errors.push_back("哈希校验失败(期望 " + mf.hashes.front().first + ")");
    fs::remove(part_path, ec);
    return false;
#else
    // 无 OpenSSL 构建:哈希能力缺失,降级为跳过校验(可用性优先)
    FALCON_LOG_WARN_STREAM(
        "[metalink] OpenSSL 不可用,跳过哈希校验: " << mf.name);
    return true;
#endif
}

} // namespace

void MetalinkHandler::download(DownloadTask::Ptr task,
                               IEventListener* listener) {
    const TaskId task_id = task->id();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        contexts_[task_id] = std::make_unique<ActiveContext>();
    }
    ActiveContext* context = find_context(task_id);
    // 上下文清理守卫:所有退出路径都会摘除
    struct ContextGuard {
        MetalinkHandler* self;
        TaskId id;
        ~ContextGuard() {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->contexts_.erase(id);
        }
    } context_guard{this, task_id};

    // 暂停/取消收口:parent 状态是权威(TaskManager 先置状态再调
    // handler->pause/cancel,与 HTTP handler 同约定),此路径必须正常
    // 返回——worker 的 catch 对一切异常置 Failed,抛异常会把 Paused
    // 覆盖成 Failed
    auto paused_or_cancelled = [&task] {
        const auto s = task->status();
        return s == TaskStatus::Paused || s == TaskStatus::Cancelled;
    };

    std::string part_path;  // 全程窄字符串;Windows 的 fs::path 隐式转换是
                            // wstring,传 const std::string& 形参 MSVC 拒绝
    try {
        const std::string xml =
            fetch_metalink_document(task->url(), task->options(), task_id,
                                    listener, context);
        if (paused_or_cancelled()) return;
        const auto files = MetalinkFileParser::parse(xml);
        if (files.size() > 1) {
            FALCON_LOG_WARN_STREAM("[metalink] 文档含 " << files.size()
                                  << " 个 file,仅取第一个");
        }
        const MetalinkFile& mf = files.front();

        // 输出路径:options.output_filename 优先,否则用文档内 name
        const std::string final_name =
            task->options().output_filename.empty()
                ? mf.name
                : task->options().output_filename;
        const fs::path dir =
            task->options().output_directory.empty()
                ? fs::current_path()
                : fs::path(task->options().output_directory);
        std::error_code ec;
        fs::create_directories(dir, ec); // 已存在时 no-op
        const fs::path final_path = dir / final_name;
        part_path = final_path.string() + ".metalink-part";
        task->set_output_path(final_path.string());

        // 逐镜像委托;失败记录换下一个,全灭才 FAILED
        std::vector<std::string> errors;
        bool published = false;

        // 阶段2:V2 引擎原生多源分段(开关默认关,门禁全过才走)。
        // kCompleted → 整文件哈希校验后发布;kFailed → 清残留后回落
        // 下方串行循环;kSuspended → parent 已暂停/取消,由既有检查
        // 点正常 return(组挂点保留供 resume)
        std::vector<std::string> v2_urls;
        if (v2_multi_source_gate(task->options(), mf, v2_urls)) {
            std::string v2_fail_reason;
            auto outcome = V2BridgeOutcome::kFailed;
            try {
                outcome = run_v2_multi_source(mf, task, listener, part_path,
                                              v2_urls, v2_fail_reason);
            } catch (const std::exception& e) {
                // 桥接内部异常转失败回落,不向 worker 抛(与方案一致:
                // worker 的 catch 会把任务置 Failed,跳过回落机会)
                v2_fail_reason = std::string("V2 桥接异常: ") + e.what();
                outcome = V2BridgeOutcome::kFailed;
                cleanup_v2_leftovers(part_path);
            }
            if (outcome == V2BridgeOutcome::kCompleted) {
                if (verify_or_discard(mf, part_path, errors)) {
                    // 发布:rename 先于 Completed(完成=可信)
                    fs::rename(part_path, final_path);
                    task->set_status(TaskStatus::Completed);
                    return;
                }
                // 哈希不符:数据已由 verify_or_discard 删除,.tmp/.ctrl
                // 残留清掉后回落逐镜像重下(重下才有校验机会)
                FALCON_LOG_WARN_STREAM(
                    "[metalink] V2 多源哈希校验失败,回落串行镜像");
                cleanup_v2_leftovers(part_path);
            } else if (outcome == V2BridgeOutcome::kFailed) {
                FALCON_LOG_WARN_STREAM("[metalink] V2 多源失败,回落串行镜像: "
                                      << v2_fail_reason);
                errors.push_back("v2-multi-source: " + v2_fail_reason);
            }
            if (paused_or_cancelled()) return;
        }

        for (const auto& mirror : mf.urls) {
            if (paused_or_cancelled()) return;

            IProtocolHandler* target =
                registry_ ? registry_->get_handler_for_url(mirror.url)
                          : nullptr;
            if (!target ||
                target == static_cast<IProtocolHandler*>(this)) {
                errors.push_back(mirror.url + ": 无可用协议处理器");
                continue;
            }

            bool mirror_ok = false;
            try {
                mirror_ok = run_mirror(context, target, task, listener,
                                       mirror.url, part_path);
            } catch (const std::exception& e) {
                errors.push_back(mirror.url + ": " + e.what());
                FALCON_LOG_WARN_STREAM(
                    "[metalink] 镜像失败,尝试下一个: " << e.what());
                std::error_code rm_ec;
                fs::remove(part_path, rm_ec);
                continue;
            }
            if (paused_or_cancelled()) return;
            // run_mirror 返回 false ⇒ parent 已暂停/取消(其内部仅有
            // 两处 false 出口,均以 parent 状态为前提),上方检查点已按
            // 暂停语义收口——不存在"非暂停的 false",无需再分支
            (void)mirror_ok;

            if (verify_or_discard(mf, part_path, errors)) {
                // 发布:rename 先于 Completed(完成=可信)
                fs::rename(part_path, final_path);
                task->set_status(TaskStatus::Completed);
                published = true;
                break;
            }
            FALCON_LOG_WARN_STREAM(
                "[metalink] 哈希校验失败,尝试下一个镜像");
        }

        if (!published) {
            if (errors.empty()) {
                throw DownloadFailedException("所有镜像均失败");
            }
            throw DownloadFailedException("所有镜像均失败: " +
                                          join_errors(errors));
        }
    } catch (const std::exception&) {
        std::error_code rm_ec;
        if (!part_path.empty()) fs::remove(part_path, rm_ec);
        // 暂停/取消途中的一切失败均按暂停语义收口(正常返回),
        // 不得把 TaskManager 已置的 Paused/Cancelled 改写成 Failed
        if (paused_or_cancelled()) return;
        throw; // worker 统一 set_error + Failed
    }
}

//------------------------------------------------------------------------------
// 阶段2:V2 引擎原生多源分段桥接
//------------------------------------------------------------------------------

namespace {

/// 桥接轮询粒度(update_progress 内建节流,实际下发频率取较大值)
constexpr auto kV2PollInterval = std::chrono::milliseconds(200);

} // namespace

bool MetalinkHandler::v2_multi_source_gate(const DownloadOptions& options,
                                           const MetalinkFile& mf,
                                           std::vector<std::string>& urls_out) {
    // 1. 进程开关:只查不启动(engine() 惰性启动推迟到桥接真正跑起来)
    if (!V2EngineHost::instance().v2_http_enabled()) return false;

    // 2. curl 专属能力 → 回退(本地复刻 V2 adapter 的回退表:同一
    //    任务的能力必须两侧等价,socks/TLS 代理 V2 判 Unsupported)
    if (parse_http_proxy(options).kind == HttpProxyKind::Unsupported) {
        return false;
    }
    if (!options.cookie_file.empty() || !options.cookie_jar.empty()) {
        return false;  // cookie 引擎(CURLOPT_COOKIEFILE/JAR)
    }
    if (!options.http_username.empty() || !options.http_password.empty()) {
        return false;  // HTTP 401 认证
    }
    if (!options.referer.empty()) {
        return false;  // Referer 头(防盗链语义),V2 不发送
    }

    // 3. V2 可拉的镜像(http/https;FTP 镜像留阶段1)≥2 才值得混源
    for (const auto& u : mf.urls) {
        const std::string scheme = scheme_of(u.url);
        if (scheme == "http" || scheme == "https") urls_out.push_back(u.url);
    }
    if (urls_out.size() < 2) return false;

    // 4/5. 无整文件哈希(或无 OpenSSL 校验能力)不做无校验混源——
    // 镜像间内容漂移只有整文件哈希门能兜底
    if (mf.hashes.empty()) return false;
#ifdef FALCON_METALINK_HAS_HASH
    return true;
#else
    return false;
#endif
}

void MetalinkHandler::cleanup_v2_leftovers(const std::string& part_path) {
    // 引擎 cancel_task/pause 只置组状态,下载命令持有的 ofstream 要等
    // 引擎线程清扫(析构命令/关 fd)才释放——Windows 上对仍被打开的
    // 文件 remove(DeleteFile)直接失败;短暂重试等引擎放手,Linux 上
    // 首轮即成功零开销
    for (const char* suffix : {".falcon.tmp", ".falcon.ctrl"}) {
        const std::string target = part_path + suffix;
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::error_code rm_ec;
            fs::remove(target, rm_ec);
            std::error_code exists_ec;
            if (!fs::exists(target, exists_ec)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
}

MetalinkHandler::V2BridgeOutcome MetalinkHandler::run_v2_multi_source(
    const MetalinkFile& /*mf*/, const DownloadTask::Ptr& parent,
    IEventListener* /*listener*/, const std::string& part_path,
    const std::vector<std::string>& urls, std::string& fail_reason) {
    auto* host = &V2EngineHost::instance();
    auto engine = host->engine();  // 此刻才惰性启动(门禁只查开关)
    auto* group_man = engine->request_group_man();
    const TaskId id = parent->id();

    // 组对齐:PAUSED 组(resume 重入)续跑,文档镜像变更则作废重建;
    // 终态组(常驻引擎按周期回收,不等周期)提前回收让同 id 可重注入
    // (如 V2 失败→阶段1 全灭→resume 重入);不存在注入新组(同一 V1
    // id、输出路径覆盖到 part 文件)
    auto* group = group_man->find_group(id);
    if (group != nullptr && group->status() == RequestGroupStatus::PAUSED) {
        if (group->uris() != urls) {
            engine->cancel_task(id);  // 镜像列表变更:旧组断点作废
            group = nullptr;
        } else if (!engine->resume_task(id)) {
            fail_reason = "V2 引擎恢复任务失败: " + std::to_string(id);
            return V2BridgeOutcome::kFailed;
        }
    } else if (group != nullptr &&
               (group->status() == RequestGroupStatus::COMPLETED ||
                group->status() == RequestGroupStatus::FAILED ||
                group->status() == RequestGroupStatus::REMOVED)) {
        group_man->purge_finished_groups();
        group = nullptr;  // 被回收组在锁外析构,指针立即失效
    } else if (group != nullptr) {
        // 同 id 组已存在且非暂停/终态:并发冲突,回落阶段1
        fail_reason = "V2 任务组状态异常(非暂停态已存在)";
        engine->cancel_task(id);
        return V2BridgeOutcome::kFailed;
    }
    if (group == nullptr) {
        const TaskId injected = engine->add_download_as(
            id, urls, parent->options(), part_path);
        if (injected == INVALID_TASK_ID) {
            fail_reason = "V2 引擎无法接受任务(ID 冲突或 URL 无效)";
            return V2BridgeOutcome::kFailed;
        }
    }

    // 桥接轮询:parent 侧控制优先,组状态为主判据(无影子,直接驱动
    // parent——update_progress 自带节流)
    while (true) {
        std::this_thread::sleep_for(kV2PollInterval);

        const auto parent_status = parent->status();
        if (parent_status == TaskStatus::Paused ||
            parent_status == TaskStatus::Cancelled) {
            // 兜底同步(正常路径 pause()/cancel() 已转发,幂等)
            if (parent_status == TaskStatus::Cancelled) {
                engine->cancel_task(id);
            } else {
                engine->pause_task(id);
            }
            // 组保持 PAUSED 供 resume 续跑;临时文件/控制文件保留
            return V2BridgeOutcome::kSuspended;
        }

        // 每轮重取:宿主停机后引擎实例会被销毁,不长期持有
        engine = host->try_engine();
        if (!engine) {
            fail_reason = "V2 引擎已停机";
            cleanup_v2_leftovers(part_path);
            return V2BridgeOutcome::kFailed;
        }
        group_man = engine->request_group_man();
        group = group_man->find_group(id);
        if (group == nullptr) {
            fail_reason = "V2 任务组丢失";
            cleanup_v2_leftovers(part_path);
            return V2BridgeOutcome::kFailed;
        }

        switch (group->status()) {
        case RequestGroupStatus::COMPLETED: {
            // 终态先同步最终进度(200ms 粒度下最后一次轮询可能落在
            // 终态分支)
            const auto progress = group->get_progress();
            parent->update_progress(progress.downloaded, progress.total,
                                    progress.speed);
            // V2 temp_extension 语义:组完成时引擎已把
            // part_path.falcon.tmp 原子改名回 part_path(控制文件同刻
            // 删除)——成品在 part_path,哈希校验+rename 发布到
            // final_path 由调用方完成
            return V2BridgeOutcome::kCompleted;
        }
        case RequestGroupStatus::FAILED: {
            fail_reason = group->error_message().empty()
                              ? "V2 多源下载失败"
                              : group->error_message();
            // 组标 REMOVED(结合 remove_group 即时摘表,立即可重注入),
            // 并删除混源临时文件——否则回落阶段1 的单镜像下载会按
            // 污染过的临时文件"续传",挫败回落重下的意义
            engine->cancel_task(id);
            cleanup_v2_leftovers(part_path);
            return V2BridgeOutcome::kFailed;
        }
        case RequestGroupStatus::PAUSED:
            // V2 侧被暂停(如宿主停机 pause_all):parent 对齐后挂起,
            // resume 经 download() 重新进入续跑
            parent->set_status(TaskStatus::Paused);
            return V2BridgeOutcome::kSuspended;
        default: {
            // WAITING/ACTIVE/REMOVED:桥接进度(组内部进度聚合)
            const auto progress = group->get_progress();
            parent->update_progress(progress.downloaded, progress.total,
                                    progress.speed);
            break;
        }
        }
    }
}

bool MetalinkHandler::run_mirror(ActiveContext* context,
                                 IProtocolHandler* target,
                                 const DownloadTask::Ptr& parent,
                                 IEventListener* listener,
                                 const std::string& mirror_url,
                                 const std::string& part_path) {
    // 影子任务:id=parent id(同一时刻每个 parent 至多一个活跃镜像),
    // options 继承,仅 output_path 换成 part 文件
    auto shadow =
        std::make_shared<DownloadTask>(parent->id(), mirror_url,
                                       parent->options());
    shadow->set_output_path(part_path);
    auto firewall =
        std::make_shared<MetalinkDelegateListener>(parent->id(), listener);
    shadow->set_listener(firewall.get());

    {
        std::lock_guard<std::mutex> lock(context->shadow_mutex);
        const auto s = parent->status();
        if (s == TaskStatus::Paused || s == TaskStatus::Cancelled) {
            return false;
        }
        context->shadow = shadow;
        context->active_target = target;
    }

    // 影子/防火墙与同步调用同栈存活;析构时摘除注册
    struct ShadowGuard {
        ActiveContext* ctx;
        ~ShadowGuard() {
            std::lock_guard<std::mutex> lock(ctx->shadow_mutex);
            ctx->shadow.reset();
            ctx->active_target = nullptr;
        }
    } shadow_guard{context};

    target->download(shadow, firewall.get()); // 同步阻塞;异常=该镜像失败

    const auto status = shadow->status();
    if (status == TaskStatus::Completed) return true;
    // parent 已暂停/取消 → false,由 download 层查 parent 状态正常收口
    const auto s = parent->status();
    if (s == TaskStatus::Paused || s == TaskStatus::Cancelled) return false;
    throw DownloadFailedException("镜像下载未完成(状态 " +
                                  std::string(to_string(status)) + ")");
}

//------------------------------------------------------------------------------
// pause / resume / cancel
//------------------------------------------------------------------------------

void MetalinkHandler::pause(DownloadTask::Ptr task) {
    // parent 状态在此置位(HTTP handler 同约定;TaskManager 不代置),
    // download 各检查点看到 Paused 后正常返回,worker 不再改写状态
    task->set_status(TaskStatus::Paused);

    // V2 多源路径:parent 已置 Paused,桥接轮询下一拍即收敛到
    // kSuspended;此处直接转发引擎尽早冻结数据流(组不存在时幂等 false)
    if (auto engine = V2EngineHost::instance().try_engine()) {
        engine->pause_task(task->id());
    }

    ActiveContext* context = find_context(task->id());
    if (!context) return;

    std::lock_guard<std::mutex> lock(context->shadow_mutex);
    if (context->shadow && context->active_target) {
        // 转发底层:中止当前镜像的数据流(半成品由外层统一清理)
        context->active_target->pause(context->shadow);
    }
}

void MetalinkHandler::resume(DownloadTask::Ptr task,
                             IEventListener* listener) {
    // TaskManager 的 resume = Paused→Pending 重排队后重新调 download(),
    // part 文件在,续传由底层 http handler 的临时文件检测承担
    download(std::move(task), listener);
}

void MetalinkHandler::cancel(DownloadTask::Ptr task) {
    task->set_status(TaskStatus::Cancelled);

    // V2 多源路径:取消即终态,组无保留价值——cancel_task 后清残留
    // (桥接轮询随后以 kSuspended 收口,发现 parent 已 Cancelled 正常
    // 返回)。V2 残留挂在 part 文件名下(output_path 是最终名)
    if (auto engine = V2EngineHost::instance().try_engine()) {
        engine->cancel_task(task->id());
    }
    cleanup_v2_leftovers(task->output_path() + ".metalink-part");

    ActiveContext* context = find_context(task->id());
    if (!context) return;

    std::lock_guard<std::mutex> lock(context->shadow_mutex);
    if (context->shadow && context->active_target) {
        context->active_target->cancel(context->shadow);
    }
}

std::unique_ptr<IProtocolHandler> create_metalink_handler() {
    return std::make_unique<MetalinkHandler>();
}

} // namespace falcon::protocols::metalink
