#include "kv/sstable.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#include "test_util.h"

namespace kv {
namespace {

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

class SSTableTest : public ::testing::Test {
 protected:
  // 写入偶数编号的键（这样奇数编号就是"中间缺失"的键），每 7 个放一个墓碑
  void BuildTable(int n) {
    Options options;
    options.block_size = 256;  // 小块，保证有很多数据块
    TableBuilder builder(path_, options);
    for (int i = 0; i < n; i += 2) {
      ValueType type = (i % 7 == 0) ? ValueType::kDeletion : ValueType::kValue;
      std::string value = type == ValueType::kValue ? "value" + std::to_string(i) : "";
      builder.Add(Key(i), type, value);
      expected_[Key(i)] = Record{Key(i), type, value};
    }
    builder.Finish();
  }

  test::TempDir tmp_;
  std::string path_ = tmp_.path() + "/000001.sst";
  std::map<std::string, Record> expected_;
};

TEST_F(SSTableTest, GetPresentDeletedAndMissing) {
  BuildTable(2000);
  auto table = Table::Open(path_);
  EXPECT_EQ(table->smallest(), Key(0));
  EXPECT_EQ(table->largest(), Key(1998));

  std::string value;
  for (const auto& [key, rec] : expected_) {
    LookupResult r = table->Get(key, &value);
    if (rec.type == ValueType::kDeletion) {
      EXPECT_EQ(r, LookupResult::kDeleted) << key;
    } else {
      ASSERT_EQ(r, LookupResult::kFound) << key;
      EXPECT_EQ(value, rec.value);
    }
  }
  // 比最小键小、两个键之间、比最大键大
  EXPECT_EQ(table->Get("a", &value), LookupResult::kNotFound);
  EXPECT_EQ(table->Get(Key(1), &value), LookupResult::kNotFound);
  EXPECT_EQ(table->Get(Key(999), &value), LookupResult::kNotFound);
  EXPECT_EQ(table->Get("zzz", &value), LookupResult::kNotFound);
}

TEST_F(SSTableTest, IterateAndSeek) {
  BuildTable(2000);
  auto table = Table::Open(path_);
  auto it = table->NewIterator();

  auto exp = expected_.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++exp) {
    ASSERT_NE(exp, expected_.end());
    EXPECT_EQ(it->key(), exp->first);
    EXPECT_EQ(it->type(), exp->second.type);
    EXPECT_EQ(it->value(), exp->second.value);
  }
  EXPECT_EQ(exp, expected_.end());

  for (const std::string& target : {std::string("a"), Key(0), Key(1), Key(1001), Key(1998), Key(1999)}) {
    it->Seek(target);
    auto lb = expected_.lower_bound(target);
    if (lb == expected_.end()) {
      EXPECT_FALSE(it->Valid()) << target;
    } else {
      ASSERT_TRUE(it->Valid()) << target;
      EXPECT_EQ(it->key(), lb->first);
    }
  }
}

TEST_F(SSTableTest, CorruptedBlockIsDetected) {
  BuildTable(200);
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(10);  // 第一个数据块内部
    f.put('\xff');
  }
  // 打开时会读第一个数据块取最小键，校验和不一致应该报错
  EXPECT_THROW(Table::Open(path_), IOError);
}

TEST_F(SSTableTest, NotAnSSTable) {
  std::ofstream(path_) << std::string(100, 'x');
  EXPECT_THROW(Table::Open(path_), IOError);
}

}  // namespace
}  // namespace kv
