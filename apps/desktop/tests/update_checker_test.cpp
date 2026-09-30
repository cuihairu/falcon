/**
 * @file update_checker_test.cpp
 * @brief UpdateChecker 版本比较纯逻辑测试（静态函数，零网络）
 *
 * 覆盖 is_newer_tag_version 全语义与 current_version 单一事实源：
 * - 三段 bump（patch/minor/major）严格更新 → 真
 * - 相等（无前缀/v/V/空白包裹）→ 假（严格更新才报）
 * - 更旧 → 假；两段式 tag（"v0.2"）按 0.2.0 参与比较
 * - 垃圾 tag（latest/1.2.x/空串/四段）解析失败 → 假，绝不误报更新
 *
 * 当前版本从 UpdateChecker::current_version() 动态推导，不硬编码——
 * 版本号升版后本文件零改动。
 *
 * check_for_updates() 本体访问真实 GitHub API（外网红线上限），
 * 不在本 target 覆盖，与 Kodo 官方域名用例同姿态取舍。
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "services/update_checker.hpp"

#include <falcon/version.hpp>

#include <gtest/gtest.h>

#include <QString>

namespace {

using falcon::desktop::UpdateChecker;

falcon::Version current_parsed()
{
    const auto parsed = falcon::Version::parse(UpdateChecker::current_version().toStdString());
    return parsed.value();
}

QString tag_of(int major, int minor, int patch)
{
    return QString("v%1.%2.%3").arg(major).arg(minor).arg(patch);
}

} // namespace

TEST(UpdateCheckerVersionTest, CurrentVersionMatchesBuildMacro)
{
    const QString current = UpdateChecker::current_version();
    EXPECT_FALSE(current.isEmpty());
    // 编译期宏是单一事实源，运行时自报必须与之逐字节一致
    EXPECT_EQ(current.toStdString(), std::string(FALCON_VERSION_STRING));
    EXPECT_TRUE(falcon::Version::parse(current.toStdString()).has_value());
}

TEST(UpdateCheckerVersionTest, NewerTagDetectedAcrossAllBumpKinds)
{
    const auto cur = current_parsed();
    EXPECT_TRUE(UpdateChecker::is_newer_tag_version(tag_of(cur.major, cur.minor, cur.patch + 1)));
    EXPECT_TRUE(UpdateChecker::is_newer_tag_version(tag_of(cur.major, cur.minor + 1, 0)));
    EXPECT_TRUE(UpdateChecker::is_newer_tag_version(tag_of(cur.major + 1, 0, 0)));
}

TEST(UpdateCheckerVersionTest, EqualTagIsNotNewerRegardlessOfPrefix)
{
    const QString current = UpdateChecker::current_version();
    // 严格更新才提示：同版本无论 v/V 前缀或空白包裹都不算新
    EXPECT_FALSE(UpdateChecker::is_newer_tag_version(current));
    EXPECT_FALSE(UpdateChecker::is_newer_tag_version(QString("v") + current));
    EXPECT_FALSE(UpdateChecker::is_newer_tag_version(QString("V") + current));
    EXPECT_FALSE(UpdateChecker::is_newer_tag_version(QLatin1String("  ") + current + QLatin1String("  ")));
}

TEST(UpdateCheckerVersionTest, OlderTagIsNotNewer)
{
    const auto cur = current_parsed();
    int major = cur.major;
    int minor = cur.minor;
    int patch = cur.patch;
    if (patch > 0) {
        patch -= 1;
    } else if (minor > 0) {
        minor -= 1;
    } else if (major > 0) {
        major -= 1;
    } else {
        GTEST_SKIP() << "current version is 0.0.0: no strictly older version exists";
    }
    EXPECT_FALSE(UpdateChecker::is_newer_tag_version(tag_of(major, minor, patch)));
}

TEST(UpdateCheckerVersionTest, TwoComponentNewerTagAccepted)
{
    // GitHub tag 常见省略 patch 形态："v0.2" 按 0.2.0 参与比较
    const auto cur = current_parsed();
    const QString two_part = QString("v%1.%2").arg(cur.major).arg(cur.minor + 1);
    EXPECT_TRUE(UpdateChecker::is_newer_tag_version(two_part));
}

TEST(UpdateCheckerVersionTest, MalformedTagsAreNeverNewer)
{
    // 解析失败的 tag（非常规版本号）必须视作无更新，绝不误报
    const QList<QString> garbage = {
        QString("latest"),
        QString("1.2.x"),
        QString("v"),
        QString(""),
        QString("v.1.2"),
        QString("1.2.3.4"),
        QString("release-2026"),
        QString("1.2.3abc"),
    };
    for (const QString& tag : garbage) {
        EXPECT_FALSE(UpdateChecker::is_newer_tag_version(tag))
            << "tag: '" << tag.toStdString() << "'";
    }
}

TEST(UpdateCheckerVersionTest, WhitespacePaddedNewerTagTrims)
{
    const auto cur = current_parsed();
    const QString padded = QString("  v%1.%2.%3\n")
                               .arg(cur.major)
                               .arg(cur.minor + 1)
                               .arg(0);
    EXPECT_TRUE(UpdateChecker::is_newer_tag_version(padded));
}
