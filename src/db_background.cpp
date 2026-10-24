// db_background.cpp —— 后台线程：刷盘、自动压缩、手动压缩，以及 MANIFEST/过期文件维护
//
// 只有一个后台线程，所以刷盘和压缩永远串行执行。这带来两个简化：
//   ① current_ 只会被后台线程修改，放锁做 I/O 期间它不会变；
//   ② RemoveObsoleteFiles 只在后台线程调用，不会误删"正在写、还没进 Version"的新文件。
// 代价：一次大压缩期间不能刷盘，写者可能因 imm_ 未刷完而停顿。
#include <filesystem>
#include <set>

#include "kv/db.h"
#include "kv/filename.h"

namespace kv {

namespace fs = std::filesystem;

void DB::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    bg_cv_.wait(lock, [this] { return shutting_down_ || HasBackgroundWork(); });
    if (shutting_down_) break;
    try {
      DoBackgroundWork(lock);
    } catch (const std::exception& e) {
      // 异常可能在放锁做 I/O 时抛出，先把锁拿回来再改共享状态
      if (!lock.owns_lock()) lock.lock();
      bg_error_ = e.what();  // 之后 HasBackgroundWork 返回 false，后台不再干活
    }
    bg_cv_.notify_all();  // 叫醒等待的写者 / WaitForIdle / CompactAll
  }
}

bool DB::HasBackgroundWork() const {
  if (!bg_error_.empty()) return false;
  return imm_ != nullptr || manual_compaction_ || NeedsCompaction(*current_, options_);
}

void DB::DoBackgroundWork(std::unique_lock<std::mutex>& lock) {
  // 刷盘优先：可能有写者正因为 imm_ 没刷完而等着
  if (imm_ != nullptr) {
    FlushImmutable(lock);
  } else if (manual_compaction_) {
    ManualCompaction(lock);
    manual_compaction_ = false;
  } else if (auto c = PickCompaction(*current_, options_, &compact_pointers_)) {
    BackgroundCompaction(lock, *c);
  }
}

// ======================== 刷盘 ========================

void DB::FlushImmutable(std::unique_lock<std::mutex>& lock) {
  std::shared_ptr<MemTable> imm = imm_;
  uint64_t number = next_file_number_++;
  lock.unlock();
  // 写 SSTable 是最耗时的部分，不持锁：这期间读者和写者都照常工作
  std::shared_ptr<FileMeta> meta = WriteLevel0Table(*imm, number);
  lock.lock();

  stats_.flushes++;
  stats_.flush_bytes += meta->file_size();
  auto version = std::make_shared<Version>(*current_);  // copy-on-write
  version->levels[0].insert(version->levels[0].begin(), std::move(meta));
  // [持久化] 顺序很重要：先让 MANIFEST 记住新文件并推进 log_number（imm 刷完后只有
  // 当前 WAL 还需要），再把新版本装上、删除旧 WAL。
  // [性能] SaveManifest 有 fsync，持锁期间读者拿 SuperVersion 会稍等（毫秒级）。
  SaveManifest(*version, log_number_);
  current_ = std::move(version);
  imm_.reset();
  RemoveObsoleteFiles();
}

std::shared_ptr<FileMeta> DB::WriteLevel0Table(const MemTable& mem, uint64_t number) {
  auto meta = std::make_shared<FileMeta>();
  meta->number = number;
  const std::string path = TableFileName(dir_, number);
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

// ======================== 压缩 ========================

void DB::BackgroundCompaction(std::unique_lock<std::mutex>& lock, const Compaction& c) {
  std::vector<std::shared_ptr<FileMeta>> outputs;
  if (c.IsTrivialMove()) {
    outputs = c.inputs;  // [性能] 下一层没有重叠：文件原样挪下去，不用重写
    stats_.trivial_moves++;
  } else {
    std::shared_ptr<const Version> base = current_;
    lock.unlock();
    // 合并是纯 I/O + CPU，不持锁。输入文件都是不可变的，并发读完全不受影响
    outputs = RunCompaction(c, *base, options_, dir_, [this] {
      std::lock_guard<std::mutex> guard(mu_);
      return next_file_number_++;
    });
    lock.lock();
    stats_.compactions++;
    for (const auto& f : outputs) stats_.compaction_bytes += f->file_size();
  }
  // 只有本线程会修改 current_，所以此刻的 current_ 就是 base，输入文件都还在
  std::shared_ptr<Version> version = ApplyCompaction(*current_, c, outputs);
  // [持久化] 先让 MANIFEST 指向新文件，再删除旧文件。中途崩溃最多留下孤儿文件
  SaveManifest(*version, MinLiveLogNumber());
  current_ = std::move(version);
  RemoveObsoleteFiles();
}

void DB::ManualCompaction(std::unique_lock<std::mutex>& lock) {
  for (int level = 0; level < kNumLevels - 1; ++level) {
    if (auto c = CompactWholeLevel(*current_, level)) BackgroundCompaction(lock, *c);
  }
}

void DB::CompactAll() {
  std::lock_guard<std::mutex> writer(write_mu_);  // 期间暂停写入
  std::unique_lock<std::mutex> lock(mu_);
  // 先把内存表交给后台刷下去，否则留在内存里的墓碑/新值不参与压缩
  if (mem_->Count() > 0) {
    bg_cv_.wait(lock, [this] { return imm_ == nullptr || !bg_error_.empty(); });
    ThrowIfBackgroundError();
    SwitchMemTable();
  }
  // 压缩交给后台线程做（它是唯一修改 current_ 的线程），这里只等它完成
  manual_compaction_ = true;
  bg_cv_.notify_all();
  bg_cv_.wait(lock, [this] { return !manual_compaction_ || !bg_error_.empty(); });
  ThrowIfBackgroundError();
}

void DB::WaitForIdle() {
  std::unique_lock<std::mutex> lock(mu_);
  // 正在做的那件活完成前，imm_/manual_compaction_/层分数都还没变，所以不会提前返回
  bg_cv_.wait(lock, [this] { return !bg_error_.empty() || !HasBackgroundWork(); });
  ThrowIfBackgroundError();
}

// ======================== MANIFEST 与文件清理（调用时持有 mu_）========================

void DB::SaveManifest(const Version& version, uint64_t log_number) {
  ManifestData data;
  data.next_file_number = next_file_number_;
  data.log_number = log_number;
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
  // 冻结表还没刷盘时，它的 WAL 也必须保留
  const uint64_t min_log = MinLiveLogNumber();
  for (const auto& entry : fs::directory_iterator(dir_)) {
    uint64_t number = 0;
    FileType type = ParseFileName(entry.path().filename().string(), &number);
    bool obsolete = (type == FileType::kLog && number < min_log) ||
                    (type == FileType::kTable && live.count(number) == 0);
    // 被删的 SSTable 可能还有读者在用：它们持有 fd，unlink 后照样能读完
    if (obsolete) fs::remove(entry.path());
  }
}

}  // namespace kv
