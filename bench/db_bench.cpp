// db_bench —— 性能基准（参考 LevelDB 的 db_bench）
//
// 用法：
//   db_bench [--num=100000] [--reads=N] [--value_size=100] [--memtable_kb=4096]
//            [--bloom=1] [--benchmarks=fillseq,fillrandom,readrandom,readmissing,scan]
//            [--dir=/path]   （默认在系统临时目录建一个，跑完删除）
//
// 每个测试输出一行 Markdown 表格：吞吐（ops/s）、平均 / p50 / p99 延迟（微秒）以及附加信息。
//   fillseq      新库，按键顺序写 num 条
//   fillrandom   新库，按随机顺序写 num 条（每个键恰好一次）
//   readrandom   随机读存在的键
//   readmissing  随机读不存在的键（夹在已有键之间，靠键范围剪不掉，只能靠布隆过滤器）
//   scan         随机起点，每次 Scan 100 条
// 读测试会先等后台刷盘/压缩完成，让文件布局稳定。数据在 OS 页缓存里，测的是"热"读。
#include <stdlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "kv/db.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Flags {
  int num = 100000;
  int reads = -1;  // -1 表示与 num 相同
  int value_size = 100;
  size_t memtable_kb = 4096;
  bool bloom = true;
  std::string benchmarks = "fillseq,fillrandom,readrandom,readmissing,scan";
  std::string dir;
};

Flags ParseFlags(int argc, char** argv) {
  Flags f;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto value = [&](const char* name) -> const char* {
      size_t len = std::strlen(name);
      return arg.compare(0, len, name) == 0 ? arg.c_str() + len : nullptr;
    };
    if (const char* v = value("--num=")) f.num = std::atoi(v);
    else if (const char* v = value("--reads=")) f.reads = std::atoi(v);
    else if (const char* v = value("--value_size=")) f.value_size = std::atoi(v);
    else if (const char* v = value("--memtable_kb=")) f.memtable_kb = static_cast<size_t>(std::atoll(v));
    else if (const char* v = value("--bloom=")) f.bloom = std::atoi(v) != 0;
    else if (const char* v = value("--benchmarks=")) f.benchmarks = v;
    else if (const char* v = value("--dir=")) f.dir = v;
    else {
      std::fprintf(stderr, "unknown flag: %s\n", argv[i]);
      std::exit(2);
    }
  }
  if (f.reads < 0) f.reads = f.num;
  return f;
}

// 延迟统计：记录每次操作的纳秒数，排序后取分位数
struct Latencies {
  std::vector<uint64_t> ns;
  double total_seconds = 0;

  double PercentileUs(double p) const {
    if (ns.empty()) return 0;
    size_t idx = std::min(ns.size() - 1, static_cast<size_t>(p * static_cast<double>(ns.size())));
    return static_cast<double>(ns[idx]) / 1000.0;
  }
  double AverageUs() const {
    if (ns.empty()) return 0;
    return std::accumulate(ns.begin(), ns.end(), 0.0) / static_cast<double>(ns.size()) / 1000.0;
  }
};

// 计时执行 n 次 op(i)
template <typename Op>
Latencies Measure(int n, Op op) {
  Latencies lat;
  lat.ns.reserve(static_cast<size_t>(n));
  auto begin = Clock::now();
  for (int i = 0; i < n; ++i) {
    auto t0 = Clock::now();
    op(i);
    lat.ns.push_back(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()));
  }
  lat.total_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
  std::sort(lat.ns.begin(), lat.ns.end());
  return lat;
}

class Benchmark {
 public:
  explicit Benchmark(const Flags& flags) : f_(flags), value_(static_cast<size_t>(flags.value_size), 'v') {
    owns_dir_ = f_.dir.empty();
    if (owns_dir_) {
      std::string tmpl = (std::filesystem::temp_directory_path() / "kvbench-XXXXXX").string();
      if (::mkdtemp(tmpl.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
      f_.dir = tmpl;
    }
    // 值用随机字节填充（不影响本引擎：没有压缩）
    std::mt19937 rng(7);
    for (char& c : value_) c = static_cast<char>('a' + rng() % 26);
  }

  ~Benchmark() {
    db_.reset();
    if (owns_dir_) std::filesystem::remove_all(f_.dir);
  }

  void Run() {
    std::printf("num=%d reads=%d value_size=%d memtable=%zuKB bloom=%d\n\n", f_.num, f_.reads,
                f_.value_size, f_.memtable_kb, f_.bloom ? 1 : 0);
    std::printf("| benchmark | ops | ops/s | avg(us) | p50(us) | p99(us) | notes |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---|\n");
    std::stringstream names(f_.benchmarks);
    std::string name;
    while (std::getline(names, name, ',')) RunOne(name);
  }

 private:
  void RunOne(const std::string& name) {
    if (name == "fillseq") {
      Fill(name, /*random=*/false);
    } else if (name == "fillrandom") {
      Fill(name, /*random=*/true);
    } else if (name == "readrandom" || name == "readmissing") {
      Read(name, /*missing=*/name == "readmissing");
    } else if (name == "scan") {
      ScanBench();
    } else {
      std::fprintf(stderr, "unknown benchmark: %s\n", name.c_str());
    }
  }

  static std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016d", i);
    return buf;
  }

  void OpenFresh() {
    db_.reset();
    std::filesystem::remove_all(f_.dir);
    kv::Options options;
    options.memtable_size = f_.memtable_kb << 10;
    options.use_bloom_filter = f_.bloom;
    db_ = kv::DB::Open(options, f_.dir);
  }

  void EnsureFilled() {
    if (db_ == nullptr) Fill("", /*random=*/true);  // 读测试前没有写测试：先静默灌一遍
  }

  void Fill(const std::string& name, bool random) {
    OpenFresh();
    std::vector<int> order(static_cast<size_t>(f_.num));
    std::iota(order.begin(), order.end(), 0);
    if (random) std::shuffle(order.begin(), order.end(), std::mt19937(42));
    Latencies lat = Measure(f_.num, [&](int i) { db_->Put(Key(order[static_cast<size_t>(i)]), value_); });
    db_->WaitForIdle();  // 后台刷盘/压缩不计入写入耗时，但要算进写放大
    if (name.empty()) return;

    kv::DBStats s = db_->GetStats();
    double wa = static_cast<double>(s.flush_bytes + s.compaction_bytes) /
                static_cast<double>(std::max<uint64_t>(1, s.user_bytes_written));
    char notes[160];
    std::snprintf(notes, sizeof(notes), "写放大 %.2f; 文件 L0/L1/L2/L3 = %d/%d/%d/%d; 压缩 %llu 次",
                  wa, db_->NumFilesAtLevel(0), db_->NumFilesAtLevel(1), db_->NumFilesAtLevel(2),
                  db_->NumFilesAtLevel(3), static_cast<unsigned long long>(s.compactions));
    Report(name, lat, notes);
  }

  void Read(const std::string& name, bool missing) {
    EnsureFilled();
    db_->WaitForIdle();
    std::mt19937 rng(1234);
    uint64_t blocks_before = db_->GetStats().block_reads;
    int found = 0;
    Latencies lat = Measure(f_.reads, [&](int) {
      std::string key = Key(static_cast<int>(rng() % static_cast<unsigned>(f_.num)));
      if (missing) key += "x";  // 介于两个已有键之间，不存在
      if (db_->Get(key)) ++found;
    });
    double blocks = static_cast<double>(db_->GetStats().block_reads - blocks_before) / f_.reads;
    char notes[96];
    std::snprintf(notes, sizeof(notes), "命中 %d/%d; 平均每次读数据块 %.3f 个", found, f_.reads, blocks);
    Report(name, lat, notes);
  }

  void ScanBench() {
    EnsureFilled();
    db_->WaitForIdle();
    std::mt19937 rng(99);
    const int scans = std::max(1, f_.reads / 100);
    size_t rows = 0;
    Latencies lat = Measure(scans, [&](int) {
      rows += db_->Scan(Key(static_cast<int>(rng() % static_cast<unsigned>(f_.num))), "", 100).size();
    });
    char notes[96];
    std::snprintf(notes, sizeof(notes), "每次 Scan 100 条; 合计 %.0f 条/s",
                  static_cast<double>(rows) / lat.total_seconds);
    Report("scan", lat, notes);
  }

  static void Report(const std::string& name, const Latencies& lat, const char* notes) {
    std::printf("| %s | %zu | %.0f | %.2f | %.2f | %.2f | %s |\n", name.c_str(), lat.ns.size(),
                static_cast<double>(lat.ns.size()) / lat.total_seconds, lat.AverageUs(),
                lat.PercentileUs(0.50), lat.PercentileUs(0.99), notes);
    std::fflush(stdout);
  }

  Flags f_;
  bool owns_dir_ = false;
  std::string value_;
  std::unique_ptr<kv::DB> db_;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    Benchmark bench(ParseFlags(argc, argv));
    bench.Run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
