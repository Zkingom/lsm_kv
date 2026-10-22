#include "kv/skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace kv {
namespace {

TEST(SkipListTest, EmptyList) {
  SkipList<int, int> list;
  EXPECT_EQ(list.Size(), 0u);
  EXPECT_EQ(list.Find(1), nullptr);
  SkipList<int, int>::Iterator it(&list);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, InsertFindAndOverwrite) {
  SkipList<std::string, std::string> list;
  EXPECT_FALSE(list.InsertOrAssign("b", "1").has_value());
  EXPECT_FALSE(list.InsertOrAssign("a", "2").has_value());

  auto old = list.InsertOrAssign("b", "3");
  ASSERT_TRUE(old.has_value());
  EXPECT_EQ(*old, "1");  // 返回被覆盖的旧值
  EXPECT_EQ(list.Size(), 2u);  // 覆盖写不增加节点数

  ASSERT_NE(list.Find("b"), nullptr);
  EXPECT_EQ(*list.Find("b"), "3");
  EXPECT_EQ(list.Find("c"), nullptr);
}

TEST(SkipListTest, IteratesInOrderAndSeeks) {
  SkipList<int, int> list;
  std::mt19937 rng(42);
  std::map<int, int> expected;
  for (int i = 0; i < 2000; ++i) {
    int k = static_cast<int>(rng() % 5000);
    list.InsertOrAssign(k, i);
    expected[k] = i;
  }
  ASSERT_EQ(list.Size(), expected.size());

  SkipList<int, int>::Iterator it(&list);
  auto exp = expected.begin();
  for (it.SeekToFirst(); it.Valid(); it.Next(), ++exp) {
    ASSERT_NE(exp, expected.end());
    EXPECT_EQ(it.key(), exp->first);
    EXPECT_EQ(it.value(), exp->second);
  }
  EXPECT_EQ(exp, expected.end());

  // Seek 定位到第一个 >= target 的键，与 std::map::lower_bound 一致
  for (int target : {-1, 0, 1234, 4999, 5000}) {
    it.Seek(target);
    auto lb = expected.lower_bound(target);
    if (lb == expected.end()) {
      EXPECT_FALSE(it.Valid());
    } else {
      ASSERT_TRUE(it.Valid());
      EXPECT_EQ(it.key(), lb->first);
    }
  }
}

}  // namespace
}  // namespace kv
