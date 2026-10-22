#include "kv/merging_iterator.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace kv {
namespace {

std::unique_ptr<Iterator> MakeIter(std::vector<Record> records) {
  return std::make_unique<VectorIterator>(std::move(records));
}

TEST(MergingIteratorTest, NewestVersionWinsAndOrderIsKept) {
  std::vector<std::unique_ptr<Iterator>> children;
  // 下标 0 最新
  children.push_back(MakeIter({{"b", ValueType::kValue, "b-new"}, {"d", ValueType::kDeletion, ""}}));
  children.push_back(MakeIter({{"a", ValueType::kValue, "a-old"}, {"b", ValueType::kValue, "b-old"},
                               {"d", ValueType::kValue, "d-old"}}));
  children.push_back(MakeIter({}));  // 空的子迭代器
  children.push_back(MakeIter({{"c", ValueType::kValue, "c"}, {"d", ValueType::kValue, "d-oldest"}}));
  MergingIterator it(std::move(children));

  std::vector<std::string> got;
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    got.push_back(it.key() + "=" + (it.type() == ValueType::kDeletion ? "<del>" : it.value()));
  }
  EXPECT_EQ(got, (std::vector<std::string>{"a=a-old", "b=b-new", "c=c", "d=<del>"}));

  it.Seek("bb");
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "c");
  it.Seek("e");
  EXPECT_FALSE(it.Valid());
}

}  // namespace
}  // namespace kv
