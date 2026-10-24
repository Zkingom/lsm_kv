// db.cpp —— 打开/恢复、写路径、读路径。后台刷盘与压缩见 db_background.cpp
#include "kv/db.h"

#include <algorithm>
#include <filesystem>

#include "kv/filename.h"
#include "kv/merging_iterator.h"

namespace kv {

namespace fs = std::filesystem;

std::unique_ptr<DB> DB::Open(const Options& options, const std::string& dir) {
  // 构造函数是私有的，不能用 make_unique
  std::unique_ptr<DB> db(new DB(options, dir));
  db->Recover();
  // 恢复完成后才启动后台线程，恢复过程不用考虑并发
  db->bg_thread_ = std::thread(&DB::BackgroundLoop, db.get());
  return db;
}

DB::DB(const Options& options, const std::string& dir)
    : options_(options),
      dir_(dir),
      mem_(std::make_shared<MemTable>()),
      current_(std::make_shared<Version>()) {}

DB::~DB() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  // 后台线程如果正在刷盘/压缩，会做完这一件再退出
  if (bg_thread_.joinable()) bg_thread_.join();
}

// ======================== 恢复 ========================

void DB::Recover() {
  // 这时还没有其它线程，但仍按"访问共享状态要持有 mu_"的约定加锁，规则统一
  std::unique_lock<std::mutex> lock(mu_);
  fs::create_directories(dir_);
  lock_ = std::make_unique<FileLock>(LockFileName(dir_));

  uint64_t min_log_number = 0;
  LoadManifest(&min_log_number);
  ReplayLogs(min_log_number);

  // [持久化] 重放出的数据直接刷成 SSTable，然后换一个全新的 WAL。
  // 这样旧 WAL（可能末尾有残缺记录）可以整个删掉，不需要在残缺处之后继续追加。
  log_number_ = next_file_number_++;
  wal_ = std::make_unique<WalWriter>(LogFileName(dir_, log_number_), options_.sync_wal);
  if (mem_->Count() > 0) {
    imm_ = std::move(mem_);
    imm_log_number_ = min_log_number;
    mem_ = std::make_shared<MemTable>();
    FlushImmutable(lock);
  } else {
    SaveManifest(*current_, log_number_);
    RemoveObsoleteFiles();
  }
}

void DB::LoadManifest(uint64_t* min_log_number) {
  std::optional<ManifestData> manifest = ReadManifest(dir_);
  if (!manifest) return;  // 全新的数据库

  next_file_number_ = manifest->next_file_number;
  *min_log_number = manifest->log_number;
  auto version = std::make_shared<Version>();
  for (const auto& [level, number] : manifest->files) {
    auto meta = std::make_shared<FileMeta>();
    meta->number = number;
    meta->table = Table::Open(TableFileName(dir_, number));
    version->levels[level].push_back(std::move(meta));
  }
  // L0 新的在前；L1+ 按键范围排序
  std::sort(version->levels[0].begin(), version->levels[0].end(),
            [](const auto& a, const auto& b) { return a->number > b->number; });
  for (int level = 1; level < kNumLevels; ++level) {
    std::sort(version->levels[level].begin(), version->levels[level].end(),
              [](const auto& a, const auto& b) { return a->smallest() < b->smallest(); });
  }
  current_ = std::move(version);
}

void DB::ReplayLogs(uint64_t min_log_number) {
  std::vector<uint64_t> logs;
  for (const auto& entry : fs::directory_iterator(dir_)) {
    uint64_t number = 0;
    FileType type = ParseFileName(entry.path().filename().string(), &number);
    if (type == FileType::kOther) continue;
    // MANIFEST 记录的 next_file_number 可能落后于磁盘上已创建的文件
    // （例如新建了 WAL 但还没来得及写 MANIFEST 就崩溃），这里修正，避免编号重复。
    next_file_number_ = std::max(next_file_number_, number + 1);
    if (type == FileType::kLog && number >= min_log_number) logs.push_back(number);
  }
  // 按编号从旧到新重放，后写的覆盖先写的
  std::sort(logs.begin(), logs.end());
  for (uint64_t number : logs) {
    ReplayWal(LogFileName(dir_, number),
              [this](ValueType type, std::string_view key, std::string_view value) {
                if (type == ValueType::kValue) {
                  mem_->Put(std::string(key), std::string(value));
                } else {
                  mem_->Delete(std::string(key));
                }
              });
  }
}

// ======================== 写路径 ========================

void DB::Put(const std::string& key, const std::string& value) {
  Write(ValueType::kValue, key, value);
}

void DB::Delete(const std::string& key) { Write(ValueType::kDeletion, key, std::string()); }

void DB::Write(ValueType type, const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> writer(write_mu_);  // 单写者：多个线程同时写会在这里排队
  std::shared_ptr<MemTable> mem;
  {
    std::unique_lock<std::mutex> lock(mu_);
    MakeRoomForWrite(lock);
    mem = mem_;
  }
  // 下面两步不持有 mu_：写 WAL 的磁盘 I/O 不会挡住读者和后台线程。
  // mem 不会在这期间被冻结——只有写者自己会切换 MemTable，而写者就是我们。
  // [持久化] 先写日志，再写内存表（原因见 db.h 顶部）
  wal_->AddRecord(type, key, value);
  if (type == ValueType::kValue) {
    mem->Put(key, value);
  } else {
    mem->Delete(key);
  }
}

void DB::MakeRoomForWrite(std::unique_lock<std::mutex>& lock) {
  while (true) {
    ThrowIfBackgroundError();
    if (mem_->ApproximateMemoryUsage() < options_.memtable_size) return;
    if (imm_ != nullptr) {
      // 上一张冻结表还没刷完，内存里不能再多一张：等后台（背压）。
      // [性能] 写入在这里停顿就是 write stall。写入速度长期超过刷盘速度时一定会发生。
      bg_cv_.wait(lock);
    } else if (current_->levels[0].size() >= options_.l0_stop_writes_trigger) {
      bg_cv_.wait(lock);  // L0 堆积太多会拖垮读，等后台压缩追上
    } else {
      SwitchMemTable();
      bg_cv_.notify_all();  // 叫醒后台线程去刷盘
    }
  }
}

// 调用时持有 write_mu_ 和 mu_
void DB::SwitchMemTable() {
  // 新 MemTable 配新 WAL。旧 WAL 要等 imm_ 刷盘成功后才能删
  uint64_t new_log = next_file_number_++;
  wal_ = std::make_unique<WalWriter>(LogFileName(dir_, new_log), options_.sync_wal);
  imm_ = std::move(mem_);
  imm_log_number_ = log_number_;
  mem_ = std::make_shared<MemTable>();
  log_number_ = new_log;
}

void DB::ThrowIfBackgroundError() const {
  if (!bg_error_.empty()) throw IOError("background error: " + bg_error_);
}

// ======================== 读路径 ========================

DB::SuperVersion DB::GetSuperVersion() const {
  // 只在锁内拷贝三个指针（几十纳秒），真正的查找在锁外进行
  std::lock_guard<std::mutex> lock(mu_);
  return SuperVersion{mem_, imm_, current_};
}

std::optional<std::string> DB::Get(const std::string& key) const {
  SuperVersion sv = GetSuperVersion();
  std::string value;
  // 从新到旧查：可写 MemTable → 冻结 MemTable → SSTable
  LookupResult r = sv.mem->Get(key, &value);
  if (r == LookupResult::kNotFound && sv.imm) r = sv.imm->Get(key, &value);
  if (r == LookupResult::kNotFound) r = sv.version->Get(key, &value);
  if (r == LookupResult::kFound) return value;
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> DB::Scan(const std::string& start,
                                                         const std::string& end,
                                                         size_t limit) const {
  SuperVersion sv = GetSuperVersion();
  // 子迭代器必须按 新→旧 排列，归并时同一个键只保留最新的
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(sv.mem->NewIterator());
  if (sv.imm) children.push_back(sv.imm->NewIterator());
  sv.version->AddIterators(start, end, &children);

  MergingIterator it(std::move(children));
  std::vector<std::pair<std::string, std::string>> result;
  for (it.Seek(start); it.Valid() && result.size() < limit; it.Next()) {
    if (!end.empty() && it.key() >= end) break;
    if (it.type() == ValueType::kDeletion) continue;  // 最新版本是墓碑：这个键已被删除
    result.emplace_back(it.key(), it.value());
  }
  return result;
}

int DB::NumFilesAtLevel(int level) const {
  return static_cast<int>(GetSuperVersion().version->levels[level].size());
}

uint64_t DB::TotalTableBytes() const {
  uint64_t total = 0;
  for (const auto& files : GetSuperVersion().version->levels) {
    for (const auto& f : files) total += f->file_size();
  }
  return total;
}

}  // namespace kv
