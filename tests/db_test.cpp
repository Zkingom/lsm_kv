#include "kv/db.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "kv/file.h"
#include "kv/filename.h"
#include "test_util.h"

namespace kv {
namespace {

std::unique_ptr<DB> OpenDB(const std::string& dir) { return DB::Open(Options(), dir); }

TEST(DBTest, OverwriteDeleteAndMissing) {
  test::TempDir tmp;
  auto db = OpenDB(tmp.path());
  EXPECT_FALSE(db->Get("a").has_value());

  db->Put("a", "1");
  db->Put("a", "2");
  EXPECT_EQ(db->Get("a"), "2");

  db->Delete("a");
  EXPECT_FALSE(db->Get("a").has_value());

  db->Delete("not-exist");  // 删除不存在的键不报错
  EXPECT_FALSE(db->Get("not-exist").has_value());

  db->Put("empty", "");  // 空值是合法值，要和"不存在"区分
  ASSERT_TRUE(db->Get("empty").has_value());
  EXPECT_EQ(*db->Get("empty"), "");
}

TEST(DBTest, ReopenRecoversFromWal) {
  test::TempDir tmp;
  {
    auto db = OpenDB(tmp.path());
    db->Put("a", "1");
    db->Put("b", "2");
    db->Put("a", "3");
    db->Delete("b");
  }
  auto db = OpenDB(tmp.path());
  EXPECT_EQ(db->Get("a"), "3");
  EXPECT_FALSE(db->Get("b").has_value());
}

TEST(DBTest, SecondOpenOfSameDirFails) {
  test::TempDir tmp;
  auto db = OpenDB(tmp.path());
  EXPECT_THROW(OpenDB(tmp.path()), IOError);
}

// 模拟"写到一半崩溃"：把 WAL 截掉最后几个字节（最后一条记录残缺）后重启
TEST(DBTest, TruncatedWalTailIsDiscarded) {
  test::TempDir tmp;
  const std::string log = LogFileName(tmp.path(), 1);
  {
    auto db = OpenDB(tmp.path());
    for (int i = 0; i < 100; ++i) db->Put("key" + std::to_string(i), "value" + std::to_string(i));
  }
  auto full_size = std::filesystem::file_size(log);
  std::filesystem::resize_file(log, full_size - 3);

  {
    auto db = OpenDB(tmp.path());
    for (int i = 0; i < 99; ++i) EXPECT_EQ(db->Get("key" + std::to_string(i)), "value" + std::to_string(i));
    EXPECT_FALSE(db->Get("key99").has_value());  // 残缺的那条被丢弃
    // 崩溃恢复后继续写，新记录必须能在下次重启时读到（说明残缺部分被物理截断了）
    db->Put("after", "crash");
  }
  auto db = OpenDB(tmp.path());
  EXPECT_EQ(db->Get("after"), "crash");
  EXPECT_EQ(db->Get("key98"), "value98");
}

int CountFiles(const std::string& dir, const std::string& suffix) {
  int n = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == suffix) ++n;
  }
  return n;
}

int TotalFiles(const DB& db) {
  int n = 0;
  for (int level = 0; level < kNumLevels; ++level) n += db.NumFilesAtLevel(level);
  return n;
}

Options SmallMemTableOptions() {
  Options options;
  options.memtable_size = 16 << 10;  // 16KB：很快就会触发刷盘
  options.block_size = 1 << 10;
  return options;
}

// Phase 3 验收：写入远超阈值的数据（多次刷盘），重启后全部能读回
TEST(DBTest, FlushToSSTableAndReadBackAfterReopen) {
  test::TempDir tmp;
  const int n = 5000;
  {
    auto db = DB::Open(SmallMemTableOptions(), tmp.path());
    for (int i = 0; i < n; ++i) db->Put("key" + std::to_string(i), "value" + std::to_string(i));
    db->WaitForIdle();  // 刷盘在后台进行
    EXPECT_GT(TotalFiles(*db), 1);
    // 刷盘成功后旧 WAL 被删除，只剩当前 MemTable 对应的那一个
    EXPECT_EQ(CountFiles(tmp.path(), ".log"), 1);
    for (int i = 0; i < n; ++i) ASSERT_EQ(db->Get("key" + std::to_string(i)), "value" + std::to_string(i));
  }
  auto db = DB::Open(SmallMemTableOptions(), tmp.path());
  db->WaitForIdle();
  for (int i = 0; i < n; ++i) ASSERT_EQ(db->Get("key" + std::to_string(i)), "value" + std::to_string(i));
  EXPECT_EQ(CountFiles(tmp.path(), ".sst"), TotalFiles(*db));  // 没有孤儿文件
}

// 新 SSTable 里的墓碑/新值，必须遮住老 SSTable 里的旧值
TEST(DBTest, NewerDataShadowsOlderSSTables) {
  test::TempDir tmp;
  auto db = DB::Open(SmallMemTableOptions(), tmp.path());
  const std::string filler(200, 'f');
  db->Put("x", "old");
  db->Put("y", "old");
  for (int i = 0; i < 200; ++i) db->Put("fill" + std::to_string(i), filler);  // 把 x,y 刷下去
  db->Put("x", "new");
  db->Delete("y");
  for (int i = 0; i < 200; ++i) db->Put("fill" + std::to_string(i), filler);  // 新值和墓碑也刷下去
  db->WaitForIdle();
  ASSERT_GE(TotalFiles(*db), 1);
  EXPECT_EQ(db->Get("x"), "new");
  EXPECT_FALSE(db->Get("y").has_value());

  db.reset();
  db = DB::Open(SmallMemTableOptions(), tmp.path());
  EXPECT_EQ(db->Get("x"), "new");
  EXPECT_FALSE(db->Get("y").has_value());
}

// 模拟刷盘写到一半崩溃：目录里有一个 MANIFEST 不认识的残缺 .sst
TEST(DBTest, OrphanTableFileIsRemovedOnOpen) {
  test::TempDir tmp;
  {
    auto db = DB::Open(SmallMemTableOptions(), tmp.path());
    db->Put("a", "1");
  }
  std::ofstream(TableFileName(tmp.path(), 999)) << "half written garbage";
  auto db = DB::Open(SmallMemTableOptions(), tmp.path());
  EXPECT_EQ(db->Get("a"), "1");
  EXPECT_FALSE(std::filesystem::exists(TableFileName(tmp.path(), 999)));
}

}  // namespace
}  // namespace kv
