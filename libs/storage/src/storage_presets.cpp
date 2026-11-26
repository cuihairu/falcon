/**
 * @file storage_presets.cpp
 * @brief 对象存储/网盘连接预设表实现
 * @author Falcon Team
 * @date 2026-10-02
 */

#include <falcon/storage/storage_presets.hpp>

namespace falcon {

std::vector<StoragePreset> storage_presets() {
    return {
        // ---------------- 对象存储（S3 兼容 XML 协议面） ----------------
        {"aws-s3", "Amazon S3", "s3", "s3",
         "AWS 官方 S3；virtual-host 域名按 bucket+region 自动拼装",
         "", "us-east-1", "Access Key ID（IAM 创建）",
         "https://console.aws.amazon.com/iam/home#/security_credentials"},
        {"cloudflare-r2", "Cloudflare R2", "s3", "s3",
         "S3 兼容 API；endpoint 需替换为账户级地址（控台 R2 页可见）",
         "https://<account_id>.r2.cloudflarestorage.com", "auto",
         "Access Key ID（R2 → Manage API Tokens）",
         "https://dash.cloudflare.com/profile/api-tokens"},
        {"backblaze-b2", "Backblaze B2", "s3", "s3",
         "S3 兼容 API；region 为桶所在区域代码（如 us-west-004）",
         "https://s3.<region>.backblazeb2.com", "us-west-004",
         "KeyID / applicationKey（App Keys 页）",
         "https://secure.backblaze.com/app_keys.htm"},
        {"wasabi", "Wasabi", "s3", "s3",
         "S3 兼容 API；region 形如 us-east-1 / eu-central-1",
         "https://s3.<region>.wasabisys.com", "us-east-1",
         "Access Key / Secret Key（Access Keys 页）",
         "https://console.wasabisys.com/#/access_keys"},
        {"google-cloud-storage", "Google Cloud Storage", "s3", "s3",
         "S3 兼容 XML API；需用 HMAC 密钥（互操作性设置里创建）",
         "https://storage.googleapis.com", "auto",
         "HMAC Access ID（Cloud Storage → 互操作性）",
         "https://console.cloud.google.com/storage/settings"},
        {"minio", "MinIO / 自建 S3 兼容", "s3", "s3",
         "任意 S3 兼容实现（MinIO/RustFS/SeaweedFS/Ceph RGW 等）；"
         "endpoint 填完整地址（含端口，如 http://nas:9000）",
         "", "", "Access Key / Secret Key（服务端账号）",
         "https://min.io/docs/minio/linux/administration.html"},

        // ---------------- 网盘（原生 WebDAV） ----------------
        {"webdav", "WebDAV（自定义）", "webdav", "webdav",
         "任意 WebDAV 端点（NAS/InfiniCloud/自建）；URL 形如 "
         "webdav://host[:port]/path，https 用 davs://",
         "", "", "按服务器账号",
         "https://datatracker.ietf.org/doc/html/rfc4918"},
        {"jianguoyun", "坚果云（原生 WebDAV）", "webdav", "webdav",
         "官方 WebDAV 端点；密码用「安全选项 → 添加应用密码」生成的应用密码",
         "https://dav.jianguoyun.com/dav", "", "登录邮箱 + 应用密码",
         "https://www.jianguoyun.com/s/downloads"},

        // ---------------- 网盘（ALIST 聚合） ----------------
        {"alist", "Alist（聚合网盘）", "webdav", "webdav",
         "自建/第三方 Alist 端点；WebDAV 密码为 Alist 账户密码，"
         "用户名 admin 或自建账户",
         "http://<alist-host>:5244/dav", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/webdav.html"},
        {"azure-blob", "Azure Blob（经 Alist）", "webdav", "webdav",
         "Azure Blob 无 S3 兼容协议，经 Alist 挂载 Azure Blob 存储后 "
         "走本预设（Alist 控制台先配置存储驱动）",
         "http://<alist-host>:5244/dav/azure", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/azure_blob.html"},
        {"google-drive", "Google Drive（经 Alist）", "webdav", "webdav",
         "Alist 挂载 Google Drive（OAuth 由 Alist 侧完成），下载路径 "
         "/dav/google",
         "http://<alist-host>:5244/dav/google", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/google_drive.html"},
        {"onedrive", "OneDrive（经 Alist）", "webdav", "webdav",
         "Alist 挂载 OneDrive（个人/商业版），下载路径 /dav/onedrive",
         "http://<alist-host>:5244/dav/onedrive", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/onedrive.html"},
        {"dropbox", "Dropbox（经 Alist）", "webdav", "webdav",
         "Alist 挂载 Dropbox，下载路径 /dav/dropbox",
         "http://<alist-host>:5244/dav/dropbox", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/dropbox.html"},
        {"pcloud", "pCloud（经 Alist）", "webdav", "webdav",
         "Alist 挂载 pCloud，下载路径 /dav/pcloud",
         "http://<alist-host>:5244/dav/pcloud", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/pcloud.html"},
        {"mega", "MEGA（经 Alist）", "webdav", "webdav",
         "Alist 挂载 MEGA（账号密码填在 Alist 存储驱动），下载路径 "
         "/dav/mega",
         "http://<alist-host>:5244/dav/mega", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/mega.html"},

        // ---------------- 国内网盘（保持既有矩阵） ----------------
        {"aliyundrive", "阿里云盘（经 Alist）", "webdav", "webdav",
         "Alist 挂载阿里云盘（refresh_token 由 Alist 侧管理）",
         "http://<alist-host>:5244/dav/aliyundrive", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/aliyundrive.html"},
        {"baidu", "百度网盘（经 Alist）", "webdav", "webdav",
         "Alist 挂载百度网盘（大文件限速由百度侧决定）",
         "http://<alist-host>:5244/dav/baidu", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/baidu.html"},
        {"tianyi", "天翼云盘（经 Alist）", "webdav", "webdav",
         "Alist 挂载天翼云盘",
         "http://<alist-host>:5244/dav/tianyi", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/189pc.html"},
        {"115", "115 网盘（经 Alist）", "webdav", "webdav",
         "Alist 挂载 115 网盘（Cookie 由 Alist 侧管理）",
         "http://<alist-host>:5244/dav/115", "", "Alist 用户名 + 密码",
         "https://alist.nn.ci/guide/drivers/115.html"},
    };
}

const StoragePreset* find_storage_preset(const std::string& id) {
    static const std::vector<StoragePreset> presets = storage_presets();
    for (const auto& preset : presets) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

} // namespace falcon
