#include "kv/db.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include "kv/filename.h"
#include "kv/merging_iterator.h"

namespace kv {

namespace fs = std::filesystem;

std::unique_ptr<DB> DB::Open(const Options& options, const std::string& dir) {
  // 构造函数是私有的，不能用 make_unique
  std::unique_ptr<DB> db(new DB(options, dir));
  db->Recover();
  return db;
}

DB::DB(const Options& options, const std::string& dir)
    : options_(options),
      dir_(dir),
      mem_(std::make_shared<MemTable>()),
      current_(std::make_shared<Version>()) {}

// ======================== 恢复 ========================

void DB::Recover() {
  fs::create_directories(dir_);
  lock_ = std::make_unique<FileLock>(LockFileName(dir_));

  uint64_t min_log_number = 0;
  LoadManifest(&min_log_number);
  ReplayLogs(min_log_number);

  // [持久化] 重放出的数据直接刷成 SSTable，然后换一个全新的 WAL。
  // 这样旧 WAL（可能末尾有残缺记录）可以整个删掉，不需要在残缺处之后继续追加。
  log_number_ = NewFileNumber();
  wal_ = std::make_unique<WalWriter>(LogFileName(dir_, log_number_), options_.sync_wal);
  if (mem_->Count() > 0) {
    imm_ = std::move(mem_);
    mem_ = std::make_shared<MemTable>();
    FlushImmutable();
  } else {
    SaveManifest(*current_);
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
  MakeRoomForWrite();
  // [持久化] 先写日志，再写内存表（原因见 db.h 顶部）
  wal_->AddRecord(type, key, value);
  if (type == ValueType::kValue) {
    mem_->Put(key, value);
  } else {
    mem_->Delete(key);
  }
}

void DB::MakeRoomForWrite() {
  if (mem_->ApproximateMemoryUsage() < options_.memtable_size) return;
  SwitchMemTable();
  // [性能] Phase 3 在写线程里同步刷盘：这次写入要等整张表写完才返回（延迟毛刺）。
  // Phase 6 改为交给后台线程。
  FlushImmutable();
}

void DB::SwitchMemTable() {
  imm_ = std::move(mem_);
  mem_ = std::make_shared<MemTable>();
  // 新 MemTable 配新 WAL。旧 WAL 要等 imm_ 刷盘成功后才能删
  log_number_ = NewFileNumber();
  wal_ = std::make_unique<WalWriter>(LogFileName(dir_, log_number_), options_.sync_wal);
}

// ======================== 刷盘 ========================

void DB::FlushImmutable() {
  std::shared_ptr<FileMeta> meta = WriteLevel0Table(*imm_);
  auto version = std::make_shared<Version>(*current_);  // copy-on-write
  version->levels[0].insert(version->levels[0].begin(), std::move(meta));

  // [持久化] 顺序很重要：先让 MANIFEST 记住新文件（同时推进 log_number），
  // 再把新版本装上、删除旧 WAL。
  SaveManifest(*version);
  current_ = std::move(version);
  imm_.reset();
  RemoveObsoleteFiles();
  // [性能] Phase 5 在刷盘后同步压缩，写线程会被阻塞更久；Phase 6 移到后台线程
  MaybeCompact();
}

std::shared_ptr<FileMeta> DB::WriteLevel0Table(const MemTable& mem) {
  auto meta = std::make_shared<FileMeta>();
  meta->number = NewFileNumber();
  const std::string path = TableFileName(dir_, meta->number);
  {
    TableBuilder builder(path, options_);
    auto it = mem.NewIterator();
    // 墓碑也要写进 SSTable：它要遮住更老文件里的同名键
    for (it->SeekToFirst(); it->Valid(); it->Next()) builder.Add(it->key(), it->type(), it->value());
    builder.Finish();  // 内部已 fsync；目录项由随后 WriteManifest 里的 SyncDir 落盘
  }
  meta->table = Table::Open(path);
  return meta;
}

void DB::SaveManifest(const Version& version) {
  ManifestData data;
  data.next_file_number = next_file_number_;
  data.log_number = log_number_;
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& file : version.levels[level]) data.files.emplace_back(level, file->number);
  }
  WriteManifest(dir_, data);
}

void DB::RemoveObsoleteFiles() {
  std::set<uint64_t> live;
  for (const auto& level : current_->levels) {
    for (const auto& file : level) live.insert(file->number);
  }
  for (const auto& entry : fs::directory_iterator(dir_)) {
    const std::string name = entry.path().filename().string();
    uint64_t number = 0;
    FileType type = ParseFileName(name, &number);
    bool obsolete = (type == FileType::kLog && number < log_number_) ||
                    (type == FileType::kTable && live.count(number) == 0);
    if (obsolete) fs::remove(entry.path());
  }
}

// ======================== 压缩 ========================

void DB::MaybeCompact() {
  while (auto c = PickCompaction(*current_, options_, &compact_pointers_)) DoCompaction(*c);
}

void DB::CompactAll() {
  // 先把内存表刷下去，否则留在内存里的墓碑/新值不参与压缩
  if (mem_->Count() > 0) {
    SwitchMemTable();
    FlushImmutable();
  }
  for (int level = 0; level < kNumLevels - 1; ++level) {
    if (auto c = CompactWholeLevel(*current_, level)) DoCompaction(*c);
  }
}

void DB::DoCompaction(const Compaction& c) {
  std::vector<std::shared_ptr<FileMeta>> outputs;
  if (c.IsTrivialMove()) {
    outputs = c.inputs;  // [性能] 下一层没有重叠：文件原样挪下去，不用重写
  } else {
    outputs = RunCompaction(c, *current_, options_, dir_, [this] { return NewFileNumber(); });
  }
  std::shared_ptr<Version> version = ApplyCompaction(*current_, c, outputs);
  // [持久化] 先让 MANIFEST 指向新文件，再删除旧文件。中途崩溃最多留下孤儿文件
  SaveManifest(*version);
  current_ = std::move(version);
  RemoveObsoleteFiles();
}

// ======================== 读路径 ========================

std::optional<std::string> DB::Get(const std::string& key) const {
  std::string value;
  // 从新到旧查：可写 MemTable → 冻结 MemTable → SSTable
  LookupResult r = mem_->Get(key, &value);
  if (r == LookupResult::kNotFound && imm_) r = imm_->Get(key, &value);
  if (r == LookupResult::kNotFound) r = current_->Get(key, &value);
  if (r == LookupResult::kFound) return value;
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> DB::Scan(const std::string& start,
                                                         const std::string& end,
                                                         size_t limit) const {
  // 子迭代器必须按 新→旧 排列，归并时同一个键只保留最新的
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem_->NewIterator());
  if (imm_) children.push_back(imm_->NewIterator());
  current_->AddIterators(start, end, &children);

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
  return static_cast<int>(current_->levels[level].size());
}

uint64_t DB::TotalTableBytes() const {
  uint64_t total = 0;
  for (const auto& files : current_->levels) {
    for (const auto& f : files) total += f->file_size();
  }
  return total;
}

}  // namespace kv
