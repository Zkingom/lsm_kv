#include "kv/db.h"

#include <gtest/gtest.h>

#include <filesystem>
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

}  // namespace
}  // namespace kv
