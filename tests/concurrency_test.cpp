// 并发压力测试：一个写线程 + 多个读线程，后台同时在刷盘和压缩。
// 数据量用环境变量 KV_STRESS_KEYS 调整（默认 2000 个键 × 5 轮）。
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kv/db.h"
#include "test_util.h"

namespace kv {
namespace {

int StressKeys() {
  const char* env = std::getenv("KV_STRESS_KEYS");
  return env != nullptr ? std::atoi(env) : 2000;
}

std::string Key(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

// 值的格式 "<key>#<轮次>"：读者能据此判断读到的值是否属于这个键、是否比已知的更旧
int RoundOf(const std::string& key, const std::string& value) {
  if (value.compare(0, key.size(), key) != 0 || value[key.size()] != '#') return -2;
  return std::atoi(value.c_str() + key.size() + 1);
}

Options StressOptions() {
  Options options;
  options.memtable_size = 32 << 10;  // 频繁刷盘，后台一直有活
  options.block_size = 1 << 10;
  options.level1_max_bytes = 128 << 10;
  options.target_file_size = 32 << 10;
  return options;
}

// 写者一轮一轮地覆盖所有键；每完成一轮，公布 completed_round。
// 读者在读之前先看 completed_round = r：此后读到的值必须存在、属于该键、轮次 >= r。
TEST(ConcurrencyTest, OneWriterManyReaders) {
  test::TempDir tmp;
  auto db = DB::Open(StressOptions(), tmp.path());
  const int keys = StressKeys(), rounds = 5, readers = 4;
  std::atomic<int> completed_round{-1};
  std::atomic<bool> done{false};
  std::atomic<int> errors{0};

  std::thread writer([&] {
    for (int r = 0; r < rounds; ++r) {
      for (int i = 0; i < keys; ++i) db->Put(Key(i), Key(i) + "#" + std::to_string(r));
      completed_round.store(r);
    }
    done.store(true);
  });

  std::vector<std::thread> reader_threads;
  for (int t = 0; t < readers; ++t) {
    reader_threads.emplace_back([&, t] {
      std::mt19937 rng(t);
      while (!done.load()) {
        int known = completed_round.load();
        int i = static_cast<int>(rng() % keys);
        auto v = db->Get(Key(i));
        if (known >= 0 && (!v || RoundOf(Key(i), *v) < known)) ++errors;
        if (v && RoundOf(Key(i), *v) < 0) ++errors;
        if (rng() % 50 == 0) {  // 偶尔做一次范围扫描：结果必须有序，且每个值属于对应的键
          auto rows = db->Scan(Key(i), Key(i + 100));
          for (size_t j = 0; j < rows.size(); ++j) {
            if (RoundOf(rows[j].first, rows[j].second) < known) ++errors;
            if (j > 0 && !(rows[j - 1].first < rows[j].first)) ++errors;
          }
          if (known >= 0 && static_cast<int>(rows.size()) != std::min(100, keys - i)) ++errors;
        }
      }
    });
  }
  writer.join();
  for (auto& t : reader_threads) t.join();
  EXPECT_EQ(errors.load(), 0);

  db->WaitForIdle();
  for (int i = 0; i < keys; ++i) ASSERT_EQ(db->Get(Key(i)), Key(i) + "#" + std::to_string(rounds - 1));
  db.reset();
  db = DB::Open(StressOptions(), tmp.path());
  EXPECT_EQ(db->Scan("", "").size(), static_cast<size_t>(keys));
}

// 多个线程同时调用 Put：内部串行化（单写者），所有写入都不能丢
TEST(ConcurrencyTest, ConcurrentWritersAreSerialized) {
  test::TempDir tmp;
  auto db = DB::Open(StressOptions(), tmp.path());
  const int threads = 4, per_thread = StressKeys() / 2;
  std::vector<std::thread> writers;
  for (int t = 0; t < threads; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < per_thread; ++i) {
        db->Put("t" + std::to_string(t) + "-" + Key(i), std::to_string(i));
        if (i % 3 == 0) db->Delete("t" + std::to_string(t) + "-" + Key(i));
      }
    });
  }
  for (auto& w : writers) w.join();
  db->CompactAll();
  for (int t = 0; t < threads; ++t) {
    for (int i = 0; i < per_thread; ++i) {
      auto v = db->Get("t" + std::to_string(t) + "-" + Key(i));
      if (i % 3 == 0) {
        ASSERT_FALSE(v.has_value());
      } else {
        ASSERT_EQ(v, std::to_string(i));
      }
    }
  }
}

}  // namespace
}  // namespace kv
