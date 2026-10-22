// 对拍测试：同一个随机操作序列同时作用于 DB 和 std::map，任何时刻结果必须一致。
// 操作次数可以用环境变量 KV_FUZZ_OPS 调整（默认 20000，内存占用很小）。
#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <random>
#include <string>

#include "kv/db.h"
#include "test_util.h"

namespace kv {
namespace {

int FuzzOps() {
  const char* env = std::getenv("KV_FUZZ_OPS");
  return env != nullptr ? std::atoi(env) : 20000;
}

struct FuzzParam {
  bool bloom;
  size_t memtable_size;
  unsigned seed;
};

class DBFuzzTest : public ::testing::TestWithParam<FuzzParam> {
 protected:
  void SetUp() override {
    options_.use_bloom_filter = GetParam().bloom;
    options_.memtable_size = GetParam().memtable_size;
    options_.block_size = 512;
    // 小容量让数据很快压到 L2、L3，覆盖多层压缩和墓碑丢弃
    options_.level1_max_bytes = 32 << 10;
    options_.target_file_size = 8 << 10;
    db_ = DB::Open(options_, tmp_.path());
  }

  std::string RandomKey() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%05u", static_cast<unsigned>(rng_() % 2000));
    return buf;
  }

  void CheckScan() {
    std::string a = RandomKey(), b = RandomKey();
    if (b < a) std::swap(a, b);
    auto got = db_->Scan(a, b);
    std::vector<std::pair<std::string, std::string>> want(model_.lower_bound(a), model_.lower_bound(b));
    ASSERT_EQ(got, want) << "scan [" << a << ", " << b << ")";
  }

  void CheckEverything() {
    auto got = db_->Scan("", "");
    std::vector<std::pair<std::string, std::string>> want(model_.begin(), model_.end());
    ASSERT_EQ(got, want);
  }

  test::TempDir tmp_;
  Options options_;
  std::unique_ptr<DB> db_;
  std::map<std::string, std::string> model_;
  std::mt19937 rng_{GetParam().seed};
};

TEST_P(DBFuzzTest, MatchesStdMap) {
  const int ops = FuzzOps();
  for (int i = 0; i < ops; ++i) {
    unsigned dice = rng_() % 100;
    std::string key = RandomKey();
    if (dice < 50) {
      std::string value(rng_() % 64, static_cast<char>('a' + rng_() % 26));
      db_->Put(key, value);
      model_[key] = value;
    } else if (dice < 70) {
      db_->Delete(key);
      model_.erase(key);
    } else if (dice < 95) {
      auto it = model_.find(key);
      auto got = db_->Get(key);
      if (it == model_.end()) {
        ASSERT_FALSE(got.has_value()) << "op " << i << " key " << key;
      } else {
        ASSERT_EQ(got, it->second) << "op " << i << " key " << key;
      }
    } else if (dice < 98) {
      CheckScan();
    } else if (dice < 99) {
      if (rng_() % 10 == 0) db_->CompactAll();
    } else {
      // 偶尔重启：验证 WAL 重放 + SSTable 加载后状态不变
      db_.reset();
      db_ = DB::Open(options_, tmp_.path());
    }
  }
  CheckEverything();
  db_.reset();
  db_ = DB::Open(options_, tmp_.path());
  CheckEverything();
}

INSTANTIATE_TEST_SUITE_P(Configs, DBFuzzTest,
                         ::testing::Values(FuzzParam{true, 8 << 10, 1}, FuzzParam{false, 8 << 10, 2},
                                           FuzzParam{true, 64 << 10, 3}, FuzzParam{true, 1 << 20, 4}),
                         [](const ::testing::TestParamInfo<FuzzParam>& info) {
                           return std::string(info.param.bloom ? "Bloom" : "NoBloom") + "_Mem" +
                                  std::to_string(info.param.memtable_size >> 10) + "KB_Seed" +
                                  std::to_string(info.param.seed);
                         });

}  // namespace
}  // namespace kv
