/**
 * @file trash_store_test.cpp
 * @brief TrashStore 单元测试（回收站记录 + 成品文件暂存，纯 C++）
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "services/trash_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

/// 临时目录（构造创建、析构递归清理；random_device 熵源防跨进程同名）
class TempDir {
public:
    TempDir()
    {
        // random_device 熵源（CI VM 时钟粒度粗，pid+时钟截断跨进程同名概率不可忽略）
        path_ = fs::temp_directory_path()
                / ("falcon_trash_test_"
                   + std::to_string(std::random_device{}()));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string store_dir() const { return (path_ / ".falcon-trash").string(); }

private:
    fs::path path_;
};

/// 在指定路径写一个已知内容的文件
void write_file(const fs::path& p, const std::string& content)
{
    std::ofstream out(p, std::ios::binary);
    out << content;
}

/// 读文件全部内容（不存在返回空串）
std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 构造一条 completed 记录（output_path 落在独立临时目录，防跨用例污染）
falcon::desktop::TrashEntry make_entry(const std::string& base_dir,
                                       std::uint64_t id, const std::string& name)
{
    falcon::desktop::TrashEntry e;
    e.id = id;
    e.url = "http://127.0.0.1/" + name;
    e.output_path = base_dir + "/" + name;
    e.file_name = name;
    e.total_bytes = 128;
    e.status = "completed";
    return e;
}

} // namespace

TEST(TrashStoreTest, AddWithFileMovesIntoTrashDirAndJsonPersists)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());

    auto src = tmp.path() / "done.bin";
    write_file(src, "hello-trash");

    auto entry = make_entry(tmp.path().string(), 42, "done.bin");
    ASSERT_TRUE(store.add(entry, src.string()));

    // 条目字段补全：文件已移入 + 回收站内路径在位
    auto list = store.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, 42u);
    EXPECT_EQ(list[0].file_in_trash, true);
    EXPECT_EQ(list[0].status, "completed");
    EXPECT_EQ(list[0].deleted_at > 0, true);

    // 原位置消失、回收站文件内容一致
    EXPECT_FALSE(fs::exists(src));
    auto in_trash = fs::path(list[0].trash_file_path);
    EXPECT_TRUE(fs::exists(in_trash));
    EXPECT_EQ(read_file(in_trash), "hello-trash");
    // 平铺命名 = <id>_<name>
    EXPECT_EQ(in_trash.filename().string(), "42_done.bin");

    // 新实例（模拟重启）从 trash.json 恢复清单
    falcon::desktop::TrashStore reopened(tmp.store_dir());
    EXPECT_EQ(reopened.size(), 1u);
    auto again = reopened.list();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].id, 42u);
    EXPECT_EQ(again[0].file_in_trash, true);
}

TEST(TrashStoreTest, RecordOnlyEntryKeepsFileAndPersists)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());

    // cancelled/failed：只记录不移动（move_file 为空）
    auto entry = make_entry(tmp.path().string(), 7, "half.bin");
    entry.status = "cancelled";
    entry.file_in_trash = false;
    ASSERT_TRUE(store.add(entry));

    auto list = store.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].file_in_trash, false);
    EXPECT_TRUE(list[0].trash_file_path.empty());

    // restore 记录型条目：无文件可移，仅摘记录
    auto restored = store.restore(7);
    EXPECT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, entry.output_path);
    EXPECT_TRUE(store.list().empty());
}

TEST(TrashStoreTest, RestoreMovesFileBackAndDedupesExistingTarget)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());

    auto src = tmp.path() / "doc.pdf";
    write_file(src, "v1");
    auto entry = make_entry(tmp.path().string(), 9, "doc.pdf");
    ASSERT_TRUE(store.add(entry, src.string()));

    // 恢复到原路径（父目录不存在时自动创建）
    auto restored = store.restore(9);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(fs::path(*restored).filename().string(), "doc.pdf");
    EXPECT_EQ(read_file(*restored), "v1");
    EXPECT_TRUE(store.list().empty());

    // 再删一次，恢复时目标已被同名文件占用 → ".1" 去重且不覆盖
    auto src2 = tmp.path() / "doc2.pdf";
    write_file(src2, "v2");
    auto entry2 = make_entry(tmp.path().string(), 10, "doc.pdf");
    entry2.output_path = *restored; // 同一目标
    ASSERT_TRUE(store.add(entry2, src2.string()));
    auto restored2 = store.restore(10);
    ASSERT_TRUE(restored2.has_value());
    EXPECT_NE(*restored2, *restored);
    EXPECT_EQ(read_file(*restored2), "v2");
    // 原文件未被覆盖
    EXPECT_EQ(read_file(*restored), "v1");
}

TEST(TrashStoreTest, RestoreUnknownIdReturnsNullopt)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());
    EXPECT_FALSE(store.restore(123).has_value());
}

TEST(TrashStoreTest, PurgeRemovesFileAndEntryUnknownIdFails)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());

    auto src = tmp.path() / "purge.bin";
    write_file(src, "bye");
    auto entry = make_entry(tmp.path().string(), 11, "purge.bin");
    ASSERT_TRUE(store.add(entry, src.string()));

    auto list = store.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(fs::exists(list[0].trash_file_path));

    EXPECT_TRUE(store.purge(11));
    EXPECT_TRUE(store.list().empty());
    EXPECT_FALSE(fs::exists(list[0].trash_file_path));

    // 再 purge 同 id / 未知 id → false
    EXPECT_FALSE(store.purge(11));
    EXPECT_FALSE(store.purge(999));
}

TEST(TrashStoreTest, ClearRemovesAllEntriesAndFiles)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());

    for (std::uint64_t id : {1u, 2u, 3u}) {
        auto src = tmp.path() / ("f" + std::to_string(id) + ".bin");
        write_file(src, std::to_string(id));
        ASSERT_TRUE(store.add(make_entry(tmp.path().string(), id,
                                         "f" + std::to_string(id) + ".bin"),
                              src.string()));
    }
    EXPECT_EQ(store.size(), 3u);

    EXPECT_EQ(store.clear(), 3);
    EXPECT_TRUE(store.list().empty());

    // 回收站目录内文件全部消失（trash.json 除外）
    for (const auto& it : fs::directory_iterator(tmp.store_dir())) {
        EXPECT_EQ(it.path().filename().string(), "trash.json");
    }
}

TEST(TrashStoreTest, PurgeExpiredKeepsFreshRemovesOld)
{
    TempDir tmp;
    falcon::desktop::TrashStore store(tmp.store_dir());

    const std::int64_t now = 1'800'000'000; // 固定时钟，确定性断言
    const std::int64_t day = 24 * 3600;

    auto old_entry = make_entry(tmp.path().string(), 1, "old.bin");
    old_entry.deleted_at = now - 8 * day; // 超 7 天
    ASSERT_TRUE(store.add(old_entry));

    auto fresh_entry = make_entry(tmp.path().string(), 2, "fresh.bin");
    fresh_entry.deleted_at = now - day; // 1 天
    ASSERT_TRUE(store.add(fresh_entry));

    auto edge_entry = make_entry(tmp.path().string(), 3, "edge.bin");
    edge_entry.deleted_at = now - 7 * day; // 恰好 7 天 = 过期（deleted + days <= now）
    ASSERT_TRUE(store.add(edge_entry));

    // days=0：不自动清理
    EXPECT_EQ(store.purge_expired(0, now), 0);
    EXPECT_EQ(store.size(), 3u);

    EXPECT_EQ(store.purge_expired(7, now), 2);
    auto rest = store.list();
    ASSERT_EQ(rest.size(), 1u);
    EXPECT_EQ(rest[0].id, 2u);
}

TEST(TrashStoreTest, CorruptManifestTreatedAsEmptyStore)
{
    TempDir tmp;
    const std::string dir = tmp.store_dir();
    fs::create_directories(dir);
    write_file(fs::path(dir) / "trash.json", "{ not-json ]");

    // 损坏清单按空回收站处理，写入后可正常恢复使用
    falcon::desktop::TrashStore store(dir);
    EXPECT_TRUE(store.list().empty());

    auto src = tmp.path() / "after.bin";
    write_file(src, "ok");
    ASSERT_TRUE(store.add(make_entry(tmp.path().string(), 5, "after.bin"),
                          src.string()));
    EXPECT_EQ(store.size(), 1u);

    falcon::desktop::TrashStore reopened(dir);
    EXPECT_EQ(reopened.size(), 1u);
}
