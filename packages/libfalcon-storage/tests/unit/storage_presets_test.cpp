/**
 * @file storage_presets_test.cpp
 * @brief 存储预设表单元测试
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 约束面：id 唯一、category/browser_protocol 取值合法、模板字段与协议
 * 面对应（S3 家族给 region/endpoint；ALIST 家族 endpoint 带 /dav 段）、
 * 查找命中与未命中行为。
 */

#include <falcon/storage/storage_presets.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

using falcon::StoragePreset;
using falcon::storage_presets;

namespace {

std::vector<StoragePreset> s3_presets() {
    auto all = storage_presets();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [](const StoragePreset& p) {
                                 return p.category != "s3";
                             }),
              all.end());
    return all;
}

} // namespace

TEST(StoragePresetsTest, IdsAreUnique) {
    const auto presets = storage_presets();
    ASSERT_FALSE(presets.empty());
    std::set<std::string> ids;
    for (const auto& preset : presets) {
        EXPECT_TRUE(ids.insert(preset.id).second) << "duplicate id: "
                                                  << preset.id;
    }
}

TEST(StoragePresetsTest, CategoryAndProtocolAreConsistent) {
    for (const auto& preset : storage_presets()) {
        ASSERT_TRUE(preset.category == "s3" || preset.category == "webdav")
            << preset.id;
        if (preset.category == "s3") {
            EXPECT_EQ(preset.browser_protocol, "s3") << preset.id;
        } else {
            EXPECT_EQ(preset.browser_protocol, "webdav") << preset.id;
        }
    }
}

TEST(StoragePresetsTest, RequiredVendorsArePresent) {
    const auto presets = storage_presets();
    auto has = [&presets](const std::string& id) {
        return std::any_of(presets.begin(), presets.end(),
                           [&id](const StoragePreset& p) {
                               return p.id == id;
                           });
    };
    // 国外对象存储矩阵
    EXPECT_TRUE(has("aws-s3"));
    EXPECT_TRUE(has("cloudflare-r2"));
    EXPECT_TRUE(has("backblaze-b2"));
    EXPECT_TRUE(has("wasabi"));
    EXPECT_TRUE(has("google-cloud-storage"));
    EXPECT_TRUE(has("minio"));
    EXPECT_TRUE(has("azure-blob"));  // 经 Alist 载体
    // 国外网盘矩阵
    EXPECT_TRUE(has("google-drive"));
    EXPECT_TRUE(has("onedrive"));
    EXPECT_TRUE(has("dropbox"));
    EXPECT_TRUE(has("pcloud"));
    EXPECT_TRUE(has("mega"));
    // 国内保持
    EXPECT_TRUE(has("aliyundrive"));
    EXPECT_TRUE(has("baidu"));
    EXPECT_TRUE(has("tianyi"));
    EXPECT_TRUE(has("115"));
    EXPECT_TRUE(has("jianguoyun"));
}

TEST(StoragePresetsTest, S3VendorEndpointTemplates) {
    auto presets = s3_presets();
    auto by_id = [&presets](const std::string& id) -> const StoragePreset& {
        auto it = std::find_if(presets.begin(), presets.end(),
                               [&id](const StoragePreset& p) {
                                   return p.id == id;
                               });
        return *it;
    };

    // AWS 官方：endpoint 空（按 bucket+region 拼 virtual-host 域名）
    EXPECT_TRUE(by_id("aws-s3").endpoint_template.empty());
    EXPECT_EQ(by_id("aws-s3").region_default, "us-east-1");

    // 兼容厂商：endpoint 模板齐全且占位段可定位
    EXPECT_NE(by_id("cloudflare-r2").endpoint_template.find("<account_id>"),
              std::string::npos);
    EXPECT_NE(by_id("backblaze-b2").endpoint_template.find("<region>"),
              std::string::npos);
    EXPECT_NE(by_id("wasabi").endpoint_template.find("<region>"),
              std::string::npos);
    EXPECT_EQ(by_id("google-cloud-storage").endpoint_template,
              "https://storage.googleapis.com");
    // MinIO/自建：endpoint 用户自填
    EXPECT_TRUE(by_id("minio").endpoint_template.empty());
}

TEST(StoragePresetsTest, AlistPresetsCarryDavPath) {
    for (const auto& preset : storage_presets()) {
        if (preset.id == "alist" || preset.id.find("azure-blob") != std::string::npos ||
            preset.id == "google-drive" || preset.id == "onedrive" ||
            preset.id == "dropbox" || preset.id == "pcloud" ||
            preset.id == "mega" || preset.id == "aliyundrive" ||
            preset.id == "baidu" || preset.id == "tianyi" ||
            preset.id == "115") {
            EXPECT_NE(preset.endpoint_template.find("/dav"), std::string::npos)
                << preset.id;
            EXPECT_EQ(preset.category, "webdav") << preset.id;
        }
    }
    // 坚果云走官方原生端点
    auto presets = storage_presets();
    auto it = std::find_if(presets.begin(), presets.end(),
                           [](const StoragePreset& p) {
                               return p.id == "jianguoyun";
                           });
    ASSERT_NE(it, presets.end());
    EXPECT_EQ(it->endpoint_template, "https://dav.jianguoyun.com/dav");
}

TEST(StoragePresetsTest, FindReturnsMatchOrMiss) {
    const auto* hit = falcon::find_storage_preset("wasabi");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->display_name, "Wasabi");

    EXPECT_EQ(falcon::find_storage_preset("nonexistent-vendor"), nullptr);
    EXPECT_EQ(falcon::find_storage_preset(""), nullptr);
}
