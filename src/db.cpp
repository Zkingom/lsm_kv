#include "kv/db.h"

#include <filesystem>

#include "kv/filename.h"

namespace kv {

namespace fs = std::filesystem;

std::unique_ptr<DB> DB::Open(const Options& options, const std::string& dir) {
  // 构造函数是私有的，不能用 make_unique
  std::unique_ptr<DB> db(new DB(options, dir));
  db->Recover();
  return db;
}

DB::DB(const Options& options, const std::string& dir) : options_(options), dir_(dir) {}

void DB::Recover() {
  fs::create_directories(dir_);
  lock_ = std::make_unique<FileLock>(LockFileName(dir_));

  const std::string log_path = LogFileName(dir_, 1);
  if (fs::exists(log_path)) {
    WalReplayResult r = ReplayWal(log_path, [this](ValueType type, std::string_view key,
                                                   std::string_view value) {
      if (type == ValueType::kValue) {
        mem_.Put(std::string(key), std::string(value));
      } else {
        mem_.Delete(std::string(key));
      }
    });
    // [持久化] 末尾有坏数据就物理截断。否则后续新记录会追加在垃圾后面，
    // 下次重放时在垃圾处停下，新记录就全丢了。
    if (r.valid_bytes < r.file_bytes) TruncateFile(log_path, r.valid_bytes);
  }
  wal_ = std::make_unique<WalWriter>(log_path, options_.sync_wal);
}

void DB::Put(const std::string& key, const std::string& value) {
  Write(ValueType::kValue, key, value);
}

void DB::Delete(const std::string& key) { Write(ValueType::kDeletion, key, std::string()); }

void DB::Write(ValueType type, const std::string& key, const std::string& value) {
  // [持久化] 先写日志，再写内存表（原因见 db.h 顶部）
  wal_->AddRecord(type, key, value);
  if (type == ValueType::kValue) {
    mem_.Put(key, value);
  } else {
    mem_.Delete(key);
  }
}

std::optional<std::string> DB::Get(const std::string& key) const {
  std::string value;
  if (mem_.Get(key, &value) == LookupResult::kFound) return value;
  return std::nullopt;
}

}  // namespace kv
