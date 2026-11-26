/**
 * @file drive_api.h
 * @brief Falcon 插件稳定 ABI（C11）
 *
 * 这是第三方网盘插件与 Falcon 宿主之间的**唯一公开契约**。
 *
 * 稳定性保证（对标 CLAP / VST3）：
 *  - 可被 C 编译器直接包含；不依赖 C++、STL、任何第三方库。
 *  - 跨界数据只用定长整数、裸指针和函数指针；字符串为 {ptr,size} 视图；
 *    集合为 {ptr,count} 视图，不使用 STL 容器。
 *  - 由插件分配、交给宿主的内存必须用宿主分配器分配，并由宿主调用
 *    host->deallocate 或插件的 free_* 回调释放；跨 CRT 不混用堆。
 *  - 每个 vtable 首字段是 abi_version；结构体尾部可追加字段。
 *  - 破坏性变更必须提升 FALCON_PLUGIN_ABI_MAJOR，入口拒绝不兼容版本。
 *  - 能力扩展走 extension(id) 查询，新增能力不改已有布局。
 *
 * 调用约定：FALCON_CALL（Windows __cdecl，其余默认）。
 * 线程约定：除标注外可在任意线程调用；插件负责 plugin_data 线程安全。
 */
#ifndef FALCON_ABI_DRIVE_API_H
#define FALCON_ABI_DRIVE_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(__GNUC__)
#  define FALCON_CALL __cdecl
#else
#  define FALCON_CALL
#endif

#define FALCON_PLUGIN_ABI_MAJOR 1u
#define FALCON_PLUGIN_ABI_MINOR 0u
#define FALCON_PLUGIN_ABI_VERSION \
    ((FALCON_PLUGIN_ABI_MAJOR << 16) | FALCON_PLUGIN_ABI_MINOR)
#define FALCON_PLUGIN_ABI_COMPATIBLE(v) \
    (((uint32_t)(v) >> 16) == FALCON_PLUGIN_ABI_MAJOR)

/* ==========================================================================
 * 基础 POD
 * ========================================================================== */

/** 字符串视图。data 可为 NULL（size 必须为 0）；不保证 NUL 结尾。 */
typedef struct falcon_str {
    const char* data;
    uint32_t size;
} falcon_str;

/** 键值对。 */
typedef struct falcon_kv {
    falcon_str key;
    falcon_str value;
} falcon_kv;

/** 键值对数组（输入或输出，语义由具体函数注明）。 */
typedef struct falcon_kv_array {
    falcon_kv* items;
    uint32_t count;
} falcon_kv_array;

/** HTTP 请求头。 */
typedef struct falcon_http_header {
    falcon_str name;
    falcon_str value;
} falcon_http_header;

/** HTTP 响应。body/error 由宿主分配，插件用完必须 host->deallocate。 */
typedef struct falcon_http_response {
    falcon_str body;
    int32_t status_code;
    falcon_str error;
} falcon_http_response;

typedef enum falcon_log_level {
    FALCON_LOG_DEBUG = 0,
    FALCON_LOG_INFO = 1,
    FALCON_LOG_WARN = 2,
    FALCON_LOG_ERROR = 3
} falcon_log_level;

/** 服务种类。一个 so 可提供多个服务描述符。 */
typedef enum falcon_service_kind {
    FALCON_SERVICE_CLOUD_STORAGE = 1,
    FALCON_SERVICE_RESOURCE_SEARCH = 2,
    FALCON_SERVICE_CONFIG_STORAGE = 3
} falcon_service_kind;

/* ==========================================================================
 * 宿主服务表：宿主持有，经注入符号传给插件
 * ========================================================================== */

typedef struct falcon_host_api {
    uint32_t abi_version;
    void* host_data;

    void* (FALCON_CALL *allocate)(void* host_data, size_t bytes);
    void (FALCON_CALL *deallocate)(void* host_data, void* ptr);

    void (FALCON_CALL *http_get)(void* host_data,
                                 falcon_str url,
                                 const falcon_http_header* headers,
                                 uint32_t header_count,
                                 int32_t timeout_seconds,
                                 falcon_http_response* out);
    void (FALCON_CALL *http_post)(void* host_data,
                                  falcon_str url,
                                  falcon_str body,
                                  const falcon_http_header* headers,
                                  uint32_t header_count,
                                  int32_t timeout_seconds,
                                  falcon_http_response* out);
    void (FALCON_CALL *log)(void* host_data,
                            falcon_log_level level,
                            falcon_str message);
    const void* (FALCON_CALL *extension)(void* host_data, falcon_str id);
} falcon_host_api;

/* ==========================================================================
 * 服务一：网盘解析
 * ========================================================================== */

typedef enum falcon_cloud_platform {
    FALCON_CLOUD_UNKNOWN = 0,
    FALCON_CLOUD_BAIDU = 1,
    FALCON_CLOUD_LANZOU = 2,
    FALCON_CLOUD_ALIBABA = 3,
    FALCON_CLOUD_TENCENT = 4,
    FALCON_CLOUD_115 = 5,
    FALCON_CLOUD_QUARK = 6,
    FALCON_CLOUD_TIANYI = 7,
    FALCON_CLOUD_PIKPAK = 8,
    FALCON_CLOUD_MEGA = 9,
    FALCON_CLOUD_GOOGLE = 10,
    FALCON_CLOUD_ONEDRIVE = 11,
    FALCON_CLOUD_DROPBOX = 12,
    FALCON_CLOUD_YANDEX = 13
} falcon_cloud_platform;

typedef struct falcon_cloud_file {
    falcon_str id;
    falcon_str name;
    uint64_t size;
    falcon_str type;
    falcon_str md5;
    falcon_str modified_time;
    falcon_str download_url;
    falcon_str share_url;
    falcon_str password;
} falcon_cloud_file;

/** files/字符串由插件分配；宿主用完调用 free_result。 */
typedef struct falcon_extraction_result {
    bool success;
    bool recognized;
    falcon_str error_message;
    falcon_str platform_name;
    uint32_t platform_type;
    falcon_cloud_file* files;
    uint32_t file_count;
} falcon_extraction_result;

typedef struct falcon_download_options {
    falcon_str auth_token;
    falcon_str refresh_token;
    falcon_str api_key;
    falcon_str api_secret;
    bool use_vip;
    falcon_str download_thread;
    int32_t timeout_seconds;
    int32_t retry_count;
} falcon_download_options;

/** 不透明网盘插件实例。 */
typedef struct falcon_drive_plugin {
    uint32_t abi_version;
    void* plugin_data;

    void (FALCON_CALL *destroy)(struct falcon_drive_plugin* self);
    falcon_str (FALCON_CALL *platform_name)(struct falcon_drive_plugin* self);
    uint32_t (FALCON_CALL *platform_type)(struct falcon_drive_plugin* self);
    bool (FALCON_CALL *can_handle)(struct falcon_drive_plugin* self, falcon_str url);
    void (FALCON_CALL *extract_share_link)(struct falcon_drive_plugin* self,
                                           falcon_str share_url,
                                           falcon_str password,
                                           falcon_extraction_result* out);
    void (FALCON_CALL *free_result)(struct falcon_drive_plugin* self,
                                    falcon_extraction_result* result);
    falcon_str (FALCON_CALL *get_download_url)(struct falcon_drive_plugin* self,
                                               falcon_str file_id,
                                               const falcon_download_options* options);
    bool (FALCON_CALL *authenticate)(struct falcon_drive_plugin* self, falcon_str token);
    uint32_t (FALCON_CALL *get_user_info)(struct falcon_drive_plugin* self,
                                          falcon_kv* out, uint32_t capacity);
    uint32_t (FALCON_CALL *get_quota_info)(struct falcon_drive_plugin* self,
                                           falcon_kv* out, uint32_t capacity);
    const void* (FALCON_CALL *extension)(struct falcon_drive_plugin* self, falcon_str id);
} falcon_drive_plugin;

typedef struct falcon_cloud_service {
    uint32_t abi_version;
    uint32_t (FALCON_CALL *plugin_count)(void* service_data);
    falcon_drive_plugin* (FALCON_CALL *create_plugin)(void* service_data,
                                                      uint32_t index,
                                                      const falcon_host_api* host);
    void* service_data;
} falcon_cloud_service;

/* ==========================================================================
 * 服务二：资源搜索
 * ========================================================================== */

typedef struct falcon_search_result {
    falcon_str title;
    falcon_str url;
    falcon_str source;
    uint64_t size;
    falcon_str type;
    int32_t seeds;
    int32_t peers;
    double confidence;
    falcon_str hash;
    falcon_str publish_date;
    falcon_kv_array metadata;
} falcon_search_result;

typedef struct falcon_search_results {
    falcon_search_result* items;
    uint32_t count;
} falcon_search_results;

typedef struct falcon_search_query {
    falcon_str keyword;
    falcon_str category;
    uint64_t min_size;
    uint64_t max_size;
    int32_t min_seeds;
    uint64_t limit;
    int32_t page;
    falcon_str sort_by;
    bool sort_desc;
} falcon_search_query;

typedef struct falcon_search_engine_config {
    falcon_str name;
    falcon_str base_url;
    falcon_str search_path;
    const falcon_kv* headers;
    uint32_t header_count;
    const falcon_kv* params;
    uint32_t param_count;
    falcon_str encoding;
    int32_t delay_ms;
    bool enabled;
    double weight;
    falcon_str response_format;
    const falcon_kv* selectors;
    uint32_t selector_count;
    falcon_str path_pattern;
} falcon_search_engine_config;

typedef struct falcon_search_engines {
    falcon_search_engine_config* items;
    uint32_t count;
} falcon_search_engines;

/** 搜索结果内存由插件分配；宿主用完调用 free_results。 */
typedef struct falcon_search_service {
    uint32_t abi_version;
    void* service_data;

    bool (FALCON_CALL *load_config)(void* service_data,
                                    falcon_str config_file,
                                    falcon_search_engines* out);
    void (FALCON_CALL *free_engines)(void* service_data,
                                     falcon_search_engines* engines);
    void (FALCON_CALL *search_one)(void* service_data,
                                   const falcon_search_engine_config* config,
                                   const falcon_search_query* query,
                                   falcon_search_results* out);
    void (FALCON_CALL *free_results)(void* service_data,
                                     falcon_search_results* results);
    bool (FALCON_CALL *is_available)(void* service_data,
                                     const falcon_search_engine_config* config);
} falcon_search_service;

/* ==========================================================================
 * 服务三：加密配置存储
 * ========================================================================== */

typedef struct falcon_cloud_config {
    falcon_str name;
    falcon_str provider;
    falcon_str access_key;
    falcon_str secret_key;
    falcon_str region;
    falcon_str bucket;
    falcon_str endpoint;
    falcon_str custom_domain;
    falcon_kv_array extra;
    int64_t created_at;
    int64_t updated_at;
} falcon_cloud_config;

typedef struct falcon_cloud_configs {
    falcon_cloud_config* items;
    uint32_t count;
} falcon_cloud_configs;

/** 不透明配置存储实例。 */
typedef struct falcon_config_service {
    uint32_t abi_version;
    void* service_data;

    void* (FALCON_CALL *create)(void* service_data);
    void (FALCON_CALL *destroy)(void* service_data, void* handle);
    bool (FALCON_CALL *initialize)(void* handle, falcon_str db_path, falcon_str master_password);
    bool (FALCON_CALL *set_master_password)(void* handle, falcon_str password);
    bool (FALCON_CALL *verify_master_password)(void* handle, falcon_str password);

    bool (FALCON_CALL *save_cloud_config)(void* handle, const falcon_cloud_config* config);
    bool (FALCON_CALL *get_cloud_config)(void* handle, falcon_str name,
                                         falcon_cloud_config* out);
    bool (FALCON_CALL *delete_cloud_config)(void* handle, falcon_str name);
    uint32_t (FALCON_CALL *list_cloud_configs)(void* handle, falcon_str* out, uint32_t capacity);
    void (FALCON_CALL *free_configs)(void* handle, falcon_cloud_configs* configs);
    uint32_t (FALCON_CALL *search_configs)(void* handle, falcon_str provider,
                                           falcon_cloud_configs* out);
    bool (FALCON_CALL *update_cloud_config)(void* handle, falcon_str name,
                                            const falcon_cloud_config* config);
    bool (FALCON_CALL *export_configs)(void* handle, falcon_str path, falcon_str password);
    bool (FALCON_CALL *import_configs)(void* handle, falcon_str path, falcon_str password);

    /** 测试专用：故障注入标志在 exe 与 so 各持一份，须经此回调同步。 */
    void (FALCON_CALL *set_injection)(void* service_data, uint32_t point, bool enabled);
} falcon_config_service;

/* ==========================================================================
 * 插件描述符与入口
 * ========================================================================== */

/** 描述符由插件 static 持有，生命周期与 so 相同。 */
typedef struct falcon_plugin_descriptor {
    uint32_t abi_version;
    uint32_t kind;              /**< falcon_service_kind */
    const char* name;
    const char* version;
    const void* service;        /**< 指向对应服务 vtable */
} falcon_plugin_descriptor;

/** 一个 so 可导出多个描述符；宿主遍历到 count 为止。 */
typedef struct falcon_plugin_manifest {
    uint32_t abi_version;
    const char* name;
    const char* version;
    uint32_t descriptor_count;
    const falcon_plugin_descriptor* const* descriptors;
} falcon_plugin_manifest;

#define FALCON_PLUGIN_ENTRY_SYMBOL "falcon_plugin_entry"
#define FALCON_PLUGIN_SET_HOST_SYMBOL "falcon_plugin_set_host"

typedef const falcon_plugin_manifest* (FALCON_CALL *falcon_plugin_entry_fn)(
    uint32_t abi_version);
typedef void (FALCON_CALL *falcon_plugin_set_host_fn)(const falcon_host_api* host);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FALCON_ABI_DRIVE_API_H */
