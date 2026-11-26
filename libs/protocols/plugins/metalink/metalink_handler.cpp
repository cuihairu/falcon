/**
 * @file metalink_handler.cpp
 * @brief Metalink 协议处理器实现
 * @author Falcon Team
 * @date 2026-09-18
 */

#include "metalink_handler.hpp"

#include "metalink_xml_error.hpp"

#include <metalink/metalink.h>
#include <expat.h>

#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>
#include <falcon/protocol_registry.hpp>
#include <falcon/protocols/fs_rename.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

namespace {

/// XML 文档格式预检:仅验证 well-formedness,错误带行列号。
/// 语义解析交给 libmetalink,但它对格式错误只回笼统的
/// METALINK_ERR_PARSER_ERROR(无位置);预检先行保住诊断契约。
void validate_xml_well_formed(const std::string& xml_text) {
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) throw std::runtime_error("metalink 解析内存分配失败");
    // 手写守卫:成功路径与异常路径都要释放 parser
    struct ParserGuard {
        XML_Parser p;
        ~ParserGuard() { XML_ParserFree(p); }
    } guard{parser};

    if (XML_Parse(parser, xml_text.data(),
                  static_cast<int>(xml_text.size()), 1) == XML_STATUS_ERROR) {
        throw XmlParseError(
            XML_ErrorString(XML_GetErrorCode(parser)),
            static_cast<std::size_t>(XML_GetCurrentLineNumber(parser)),
            static_cast<std::size_t>(XML_GetCurrentColumnNumber(parser)));
    }
}

/// libmetalink 错误码 → 中文错误消息(runtime_error)
[[noreturn]] void throw_metalink_error(int error_code) {
    switch (error_code) {
    case METALINK_ERR_BAD_ALLOC:
        throw std::runtime_error("metalink 解析内存分配失败");
    case METALINK_ERR_MISSING_REQUIRED_ATTR:
        throw std::runtime_error("file 元素缺少 name 属性");
    case METALINK_ERR_PARSER_ERROR:
        // 格式错误已被预检拦截,理论上到不了;仍映射为解析异常
        throw XmlParseError("metalink XML 解析失败", 0, 0);
    default:
        throw std::runtime_error(std::string("metalink 解析失败: ") +
                                 metalink_strerror(error_code));
    }
}

/// RAII 持有 libmetalink 解析结果
struct MetalinkResultGuard {
    metalink_t* doc = nullptr;
    ~MetalinkResultGuard() {
        if (doc) metalink_delete(doc);
    }
};

} // namespace

std::vector<MetalinkFile> MetalinkFileParser::parse(
    const std::string& xml_text) {
    // ① 格式层:well-formed 预检(行列号诊断契约)
    validate_xml_well_formed(xml_text);

    // ② 语义层:libmetalink(RFC 5854 与 Metalink3 双版本,aria2 同款)
    MetalinkResultGuard result;
    const int rc = metalink_parse_memory(
        xml_text.data(), xml_text.size(), &result.doc);
    if (rc != 0) throw_metalink_error(rc);

    const bool known_version = result.doc->version == METALINK_VERSION_3 ||
                               result.doc->version == METALINK_VERSION_4;

    // ③ 字段映射:库结构 → MetalinkFile DTO
    std::vector<MetalinkFile> files;
    for (metalink_file_t** fp = result.doc->files; fp && *fp; ++fp) {
        metalink_file_t* f = *fp;
        MetalinkFile mf;
        mf.name = f->name ? f->name : "";
        // 库已把不安全文件名(穿越/控制字符/盘符)的 file 整个跳过;
        // 二次检查兜住库放行但我们契约更严的形态(嵌入 '/' 与反斜杠)
        if (!is_safe_filename(mf.name)) {
            throw std::runtime_error("file name 含路径穿越或分隔符: " +
                                     mf.name);
        }
        // 库对 size 已做垃圾值/负数/溢出回落 0
        mf.size = f->size > 0 ? static_cast<std::uint64_t>(f->size) : 0;

        for (metalink_checksum_t** cp = f->checksums; cp && *cp; ++cp) {
            HashAlgorithm algo;
            const char* type = (*cp)->type;
            if (!type || !hash_type_to_algorithm(type, algo)) continue;
            std::string hex = trim((*cp)->hash ? (*cp)->hash : "");
            if (hex.empty()) continue;
            mf.hashes.emplace_back(std::move(hex), algo);
        }

        for (metalink_resource_t** rp = f->resources; rp && *rp; ++rp) {
            metalink_resource_t* r = *rp;
            MetalinkUrl mu;
            mu.url = trim(r->url ? r->url : "");
            if (mu.url.empty()) continue;
            if (r->location) mu.location = r->location;
            if (r->type) mu.type = r->type;
            // v4 缺省 priority 的库内哨兵 999999 → kNoPriority;
            // v3 由库换算 priority = 1000000 - preference(值域
            // 999901..1000000,不会撞哨兵)
            mu.priority = (r->priority == 999999)
                              ? kNoPriority
                              : static_cast<int>(r->priority);
            mu.preference = r->preference;

            // 只保留委托模式下可下载的镜像:http/ftp;type 显式标注为
            // 其他协议(bittorrent 等)的条目跳过。注意 v4 文档的
            // type 属性 libmetalink 不解析,过滤按 scheme 兜底
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
        if (!known_version) {
            // 库对无命名空间/未知命名空间的文档整体静默跳过
            throw std::runtime_error(
                "不是有效的 metalink 文档(根元素缺失或不是 metalink)");
        }
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
                // 发布:rename 先于 Completed(完成=可信)。校验过的产物
                // 遇到 Windows 杀毒/索引的瞬时句柄占用,重试吞掉窗口而
                // 不是把这一个镜像直接判失败换下一家重下
                const auto rr =
                    protocols::detail::rename_with_retry(part_path,
                                                         final_path);
                if (!rr.ok) {
                    throw FileIOException("[metalink] 发布改名失败: " +
                                          part_path + " -> " +
                                          final_path.string() + " (" +
                                          rr.error + ")");
                }
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
