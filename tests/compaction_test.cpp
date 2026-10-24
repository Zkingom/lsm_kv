#include <gtest/gtest.h>

#include <string>

#include "kv/db.h"
#include "test_util.h"

namespace kv {
namespace {

Options SmallOptions() {
  Options options;
  options.memtable_size = 32 << 10;
  options.block_size = 1 << 10;
  options.level1_max_bytes = 128 << 10;
  options.target_file_size = 32 << 10;
  return options;
}

int TotalFiles(const DB& db) {
  int n = 0;
  for (int level = 0; level < kNumLevels; ++level) n += db.NumFilesAtLevel(level);
  return n;
}

std::string Key(int i) { return "key" + std::to_string(i); }

// Phase 5 验收：大量覆盖写后，文件数和总大小都远小于"不压缩"时的规模
TEST(CompactionTest, OverwritesShrinkFilesAndBytes) {
  test::TempDir tmp;
  auto db = DB::Open(SmallOptions(), tmp.path());
  const int keys = 1000, rounds = 20;
  const std::string value(100, 'v');
  uint64_t written = 0;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < keys; ++i) {
      db->Put(Key(i), value + std::to_string(r));
      written += Key(i).size() + value.size();
    }
  }
  db->WaitForIdle();  // 压缩在后台进行，等它做完再看文件布局
  uint64_t live = written / rounds;  // 只有最后一轮的数据是活的
  // 不压缩的话 SSTable 总量约等于 written（~2.2MB）、文件数 ~70 个
  EXPECT_LT(db->TotalTableBytes(), 3 * live);
  EXPECT_LT(TotalFiles(*db), 15);
  EXPECT_GT(db->NumFilesAtLevel(1) + db->NumFilesAtLevel(2), 0);

  for (int i = 0; i < keys; ++i) ASSERT_EQ(db->Get(Key(i)), value + std::to_string(rounds - 1));
}

// 墓碑只有压到最底层（下面再没有旧版本）时才会被真正丢弃
TEST(CompactionTest, TombstonesAreDroppedAtBottomLevel) {
  test::TempDir tmp;
  auto db = DB::Open(SmallOptions(), tmp.path());
  for (int i = 0; i < 3000; ++i) db->Put(Key(i), std::string(50, 'x'));
  db->CompactAll();
  ASSERT_GT(db->NumFilesAtLevel(kNumLevels - 1), 0);

  for (int i = 0; i < 3000; ++i) db->Delete(Key(i));
  // 删除后旧值不能"复活"——无论墓碑停在哪一层
  for (int i = 0; i < 3000; i += 37) ASSERT_FALSE(db->Get(Key(i)).has_value());

  db->CompactAll();
  EXPECT_EQ(TotalFiles(*db), 0);  // 值和墓碑同归于尽
  EXPECT_TRUE(db->Scan("", "").empty());

  db.reset();
  db = DB::Open(SmallOptions(), tmp.path());
  EXPECT_FALSE(db->Get(Key(5)).has_value());
}

TEST(CompactionTest, LevelsStayNonOverlappingAndDataSurvivesReopen) {
  test::TempDir tmp;
  {
    auto db = DB::Open(SmallOptions(), tmp.path());
    for (int i = 0; i < 20000; ++i) db->Put(Key(i * 7919 % 20000), std::to_string(i));
    db->WaitForIdle();
    EXPECT_LE(db->NumFilesAtLevel(0), SmallOptions().l0_compaction_trigger);
  }
  auto db = DB::Open(SmallOptions(), tmp.path());
  auto all = db->Scan("", "");
  ASSERT_EQ(all.size(), 20000u);
  for (size_t i = 1; i < all.size(); ++i) ASSERT_LT(all[i - 1].first, all[i].first);
}

}  // namespace
}  // namespace kv
