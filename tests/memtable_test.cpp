#include "kv/memtable.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace kv {
namespace {

TEST(MemTableTest, PutGetOverwrite) {
  MemTable mem;
  std::string value;
  EXPECT_EQ(mem.Get("k", &value), LookupResult::kNotFound);

  mem.Put("k", "v1");
  ASSERT_EQ(mem.Get("k", &value), LookupResult::kFound);
  EXPECT_EQ(value, "v1");

  mem.Put("k", "v2");
  ASSERT_EQ(mem.Get("k", &value), LookupResult::kFound);
  EXPECT_EQ(value, "v2");
  EXPECT_EQ(mem.Count(), 1u);
}

TEST(MemTableTest, DeleteWritesTombstone) {
  MemTable mem;
  std::string value;
  mem.Put("k", "v");
  mem.Delete("k");
  EXPECT_EQ(mem.Get("k", &value), LookupResult::kDeleted);

  // 删除一个从没写过的键，也要留下墓碑（它可能存在于更老的 SSTable）
  mem.Delete("never");
  EXPECT_EQ(mem.Get("never", &value), LookupResult::kDeleted);

  // 删除后再写，墓碑被新值覆盖
  mem.Put("k", "again");
  ASSERT_EQ(mem.Get("k", &value), LookupResult::kFound);
  EXPECT_EQ(value, "again");
}

TEST(MemTableTest, IteratorIsOrderedAndIncludesTombstones) {
  MemTable mem;
  mem.Put("c", "3");
  mem.Put("a", "1");
  mem.Delete("b");
  mem.Put("d", "4");

  auto it = mem.NewIterator();
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) keys.push_back(it->key());
  EXPECT_EQ(keys, (std::vector<std::string>{"a", "b", "c", "d"}));

  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  EXPECT_EQ(it->type(), ValueType::kDeletion);
}

TEST(MemTableTest, MemoryUsageTracksOverwrites) {
  MemTable mem;
  mem.Put("key", std::string(100, 'x'));
  size_t after_first = mem.ApproximateMemoryUsage();
  EXPECT_GE(after_first, 103u);

  // 用更短的值覆盖，占用应该下降而不是累加
  mem.Put("key", "y");
  EXPECT_LT(mem.ApproximateMemoryUsage(), after_first);
}

}  // namespace
}  // namespace kv
