#include "kv/wal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kv/crc32.h"
#include "test_util.h"

namespace kv {
namespace {

TEST(Crc32Test, KnownVectors) {
  // 标准测试向量：CRC-32/IEEE("123456789") = 0xCBF43926
  EXPECT_EQ(Crc32("123456789"), 0xCBF43926u);
  EXPECT_EQ(Crc32(""), 0u);
  EXPECT_NE(Crc32("a"), Crc32("b"));
}

struct Op {
  ValueType type;
  std::string key;
  std::string value;
  bool operator==(const Op&) const = default;
};

std::vector<Op> ReplayAll(const std::string& path, WalReplayResult* result) {
  std::vector<Op> ops;
  *result = ReplayWal(path, [&](ValueType t, std::string_view k, std::string_view v) {
    ops.push_back(Op{t, std::string(k), std::string(v)});
  });
  return ops;
}

class WalTest : public ::testing::Test {
 protected:
  void WriteOps() {
    WalWriter writer(path_, /*sync_each_write=*/false);
    for (const Op& op : ops_) writer.AddRecord(op.type, op.key, op.value);
  }

  test::TempDir tmp_;
  std::string path_ = tmp_.path() + "/test.log";
  std::vector<Op> ops_ = {
      {ValueType::kValue, "k1", "v1"},
      {ValueType::kDeletion, "k2", ""},
      {ValueType::kValue, "", "empty key"},
      {ValueType::kValue, "k3", std::string(1000, 'x')},
  };
};

TEST_F(WalTest, RoundTrip) {
  WriteOps();
  WalReplayResult r;
  EXPECT_EQ(ReplayAll(path_, &r), ops_);
  EXPECT_EQ(r.valid_bytes, r.file_bytes);
  EXPECT_EQ(r.records, ops_.size());
}

// 在每一个可能的字节位置截断：重放结果必须恰好是"完整写完的那些记录"
TEST_F(WalTest, TruncationAtEveryOffset) {
  WriteOps();
  std::string full = ReadFileToString(path_);
  // 先算出每条记录结束的位置
  std::vector<uint64_t> ends;
  {
    WalWriter w(tmp_.path() + "/probe.log", false);
    for (const Op& op : ops_) {
      w.AddRecord(op.type, op.key, op.value);
      ends.push_back(w.Size());
    }
  }

  for (size_t cut = 0; cut <= full.size(); ++cut) {
    std::filesystem::resize_file(path_, cut);
    WalReplayResult r;
    auto got = ReplayAll(path_, &r);
    size_t complete = 0;
    while (complete < ends.size() && ends[complete] <= cut) ++complete;
    ASSERT_EQ(got.size(), complete) << "cut=" << cut;
    EXPECT_TRUE(std::equal(got.begin(), got.end(), ops_.begin()));
    EXPECT_EQ(r.valid_bytes, complete == 0 ? 0 : ends[complete - 1]);
    // 恢复原文件，进入下一个截断点
    std::ofstream(path_, std::ios::binary | std::ios::trunc) << full;
  }
}

TEST_F(WalTest, CorruptedRecordStopsReplay) {
  WriteOps();
  std::string data = ReadFileToString(path_);
  data[data.size() - 5] ^= 0x01;  // 翻转最后一条记录里的一个比特
  std::ofstream(path_, std::ios::binary | std::ios::trunc) << data;

  WalReplayResult r;
  auto got = ReplayAll(path_, &r);
  EXPECT_EQ(got.size(), ops_.size() - 1);
  EXPECT_LT(r.valid_bytes, r.file_bytes);
}

}  // namespace
}  // namespace kv
