// aria2 JSON → TaskSnapshot/GlobalStats 转换层单元测试（纯函数，无 IO）

#include "rpc/aria2_snapshots.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;
using falcon::daemon::rpc::snapshot_from_status_json;
using falcon::daemon::rpc::snapshots_from_status_array;
using falcon::daemon::rpc::stats_from_global_stat_json;
using falcon::daemon::rpc::task_id_from_gid;
using falcon::daemon::rpc::task_status_from_aria2_string;

json make_status() {
    return json{
        {"gid", "000000000000002a"},
        {"status", "active"},
        {"priority", 2},
        {"totalLength", "1000"},
        {"completedLength", "250"},
        {"downloadSpeed", "512"},
        {"errorMessage", ""},
        {"files", json::array({json{
            {"path", "/downloads/file.bin"},
            {"length", "1000"},
            {"completedLength", "250"},
            {"selected", "true"},
            {"uris", json::array({json{{"uri", "http://example.com/file.bin"},
                                       {"status", "used"}}})},
        }})},
    };
}

TEST(Aria2SnapshotsTest, ParsesFullStatusObject) {
    auto snap = snapshot_from_status_json(make_status());
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->id, 42u);
    EXPECT_EQ(snap->status, falcon::TaskStatus::Downloading);
    EXPECT_EQ(snap->url, "http://example.com/file.bin");
    EXPECT_EQ(snap->output_path, "/downloads/file.bin");
    EXPECT_EQ(snap->total_bytes, 1000u);
    EXPECT_EQ(snap->downloaded_bytes, 250u);
    EXPECT_EQ(snap->speed, 512u);
    EXPECT_DOUBLE_EQ(snap->progress, 0.25);
    EXPECT_EQ(snap->priority, falcon::TaskPriority::High);
    EXPECT_FALSE(snap->is_finished());
}

TEST(Aria2SnapshotsTest, MapsAllAria2StatusStrings) {
    EXPECT_EQ(task_status_from_aria2_string("active"), falcon::TaskStatus::Downloading);
    EXPECT_EQ(task_status_from_aria2_string("waiting"), falcon::TaskStatus::Pending);
    EXPECT_EQ(task_status_from_aria2_string("paused"), falcon::TaskStatus::Paused);
    EXPECT_EQ(task_status_from_aria2_string("complete"), falcon::TaskStatus::Completed);
    EXPECT_EQ(task_status_from_aria2_string("error"), falcon::TaskStatus::Failed);
    EXPECT_EQ(task_status_from_aria2_string("removed"), falcon::TaskStatus::Cancelled);
    EXPECT_FALSE(task_status_from_aria2_string("bogus").has_value());
}

TEST(Aria2SnapshotsTest, CompletedWithoutProgressDataIsFull) {
    auto status = make_status();
    status["status"] = "complete";
    status["completedLength"] = "1000";
    auto snap = snapshot_from_status_json(status);
    ASSERT_TRUE(snap.has_value());
    EXPECT_DOUBLE_EQ(snap->progress, 1.0);
    EXPECT_TRUE(snap->is_finished());

    // complete 但长度为 0（未知大小）也应显示完成
    status["totalLength"] = "0";
    status["completedLength"] = "0";
    snap = snapshot_from_status_json(status);
    ASSERT_TRUE(snap.has_value());
    EXPECT_DOUBLE_EQ(snap->progress, 1.0);
}

TEST(Aria2SnapshotsTest, ErrorStatusCarriesMessage) {
    auto status = make_status();
    status["status"] = "error";
    status["errorMessage"] = "host unreachable";
    auto snap = snapshot_from_status_json(status);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->status, falcon::TaskStatus::Failed);
    EXPECT_EQ(snap->error_message, "host unreachable");
    EXPECT_TRUE(snap->is_finished());
}

TEST(Aria2SnapshotsTest, RejectsMalformedObjects) {
    EXPECT_FALSE(snapshot_from_status_json(json::array()).has_value());
    EXPECT_FALSE(snapshot_from_status_json(json{{"status", "active"}}).has_value());
    EXPECT_FALSE(snapshot_from_status_json(json{{"gid", "zzzz", "status", "active"}}).has_value());

    auto bad_status = make_status();
    bad_status["status"] = "???";
    EXPECT_FALSE(snapshot_from_status_json(bad_status).has_value());
}

TEST(Aria2SnapshotsTest, ParsesArraysAndSkipsBadEntries) {
    auto good = make_status();
    auto arr = json::array({good, json{{"gid", "no-status"}}, "garbage"});
    auto snaps = snapshots_from_status_array(arr);
    ASSERT_EQ(snaps.size(), 1u);
    EXPECT_EQ(snaps[0].id, 42u);

    // 非数组输入 → 空列表
    EXPECT_TRUE(snapshots_from_status_array(json::object()).empty());
}

TEST(Aria2SnapshotsTest, ToleratesNumericAndEmptyFields) {
    auto status = make_status();
    status["totalLength"] = 4096;       // 数字而非字符串
    status["completedLength"] = nullptr; // 缺失按 0
    status["downloadSpeed"] = "";
    auto snap = snapshot_from_status_json(status);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->total_bytes, 4096u);
    EXPECT_EQ(snap->downloaded_bytes, 0u);
    EXPECT_EQ(snap->speed, 0u);
}

TEST(Aria2SnapshotsTest, ParsesGlobalStat) {
    auto stats = stats_from_global_stat_json(json{
        {"downloadSpeed", "2048"},
        {"numActive", "2"},
        {"numWaiting", "5"},
        {"numStopped", "11"},
    });
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->download_speed, 2048u);
    EXPECT_EQ(stats->active_tasks, 2u);
    EXPECT_EQ(stats->waiting_tasks, 5u);
    EXPECT_EQ(stats->stopped_tasks, 11u);
    EXPECT_EQ(stats->total_tasks(), 18u);

    EXPECT_FALSE(stats_from_global_stat_json(json::array()).has_value());

    // 空对象合法：字段缺失按 0
    auto empty = stats_from_global_stat_json(json::object());
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->total_tasks(), 0u);
}

TEST(Aria2SnapshotsTest, ParsesGidForms) {
    EXPECT_EQ(task_id_from_gid("00000000000000ff"), falcon::TaskId{255});
    EXPECT_EQ(task_id_from_gid("ff"), falcon::TaskId{255});
    EXPECT_EQ(task_id_from_gid("0xFF"), falcon::TaskId{255});
    EXPECT_FALSE(task_id_from_gid("0000000000000000").has_value());
    EXPECT_FALSE(task_id_from_gid("12345678901234567").has_value()); // >16 位
    EXPECT_FALSE(task_id_from_gid("xyz").has_value());
    EXPECT_FALSE(task_id_from_gid("").has_value());
}

} // namespace
