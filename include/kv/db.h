// db.h —— 对外接口
//
// 对应组件：LevelDB include/leveldb/db.h + db/db_impl.h。
// 面试可能问：
//   - Get 为什么返回 std::optional 而不是空字符串？
//     答：空字符串是合法的值，必须和"键不存在"区分开。
//   - 写入的顺序为什么是"先 WAL 后 MemTable"？反过来会怎样？
//     答：反过来的话，写完内存还没写日志时崩溃，这条数据可能已经被别的读者读到，
//     重启后却消失了（读到了"不存在的历史"）。先写日志保证：任何被看见过的写入都能恢复。
//   - 刷盘过程中任意时刻崩溃，数据会丢吗？
//     答：不会。顺序是 ① 写完并 fsync SSTable ② 原子更新 MANIFEST ③ 删除旧 WAL。
//     ①②之间崩溃：SSTable 是孤儿（启动时删掉），WAL 还在，重放即可；
//     ②③之间崩溃：旧 WAL 编号小于 MANIFEST 的 log_number，启动时被识别为过期并删除。
//
// 并发模型（Phase 6）：
//   - 单写者：所有 Put/Delete 先拿 write_mu_，同一时刻只有一个线程在写。
//   - 多读者：Get/Scan 只在 mu_ 下拷贝三个 shared_ptr（mem_、imm_、current_，合起来
//     相当于 RocksDB 的 SuperVersion），然后立刻放锁去读。MemTable 自带读写锁；
//     冻结表、SSTable、Version 都是不可变的，读它们不需要任何锁。
//   - 一个后台线程：负责刷盘和压缩。耗时的文件 I/O 都在放开 mu_ 之后做。
//   - 加锁顺序固定为 write_mu_ → mu_ → MemTable 内部锁，避免死锁。
//
//   - 面试可能问：读者读到一半，后台把它正在读的 SSTable 删了怎么办？
//     答：读者手里的 shared_ptr<Version> 让 Table 对象（及其打开的 fd）一直活着。
//     Linux 上 unlink 一个仍被打开的文件，只是删掉目录项，数据要等最后一个 fd 关闭才释放。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kv/compaction.h"
#include "kv/file.h"
#include "kv/memtable.h"
#include "kv/options.h"
#include "kv/version.h"
#include "kv/wal.h"

namespace kv {

// 运行统计。写放大 ≈ (flush_bytes + compaction_bytes) / user_bytes_written
struct DBStats {
  uint64_t user_bytes_written = 0;  // 用户 Put/Delete 的键+值字节数
  uint64_t flush_bytes = 0;         // 刷盘写出的 SSTable 字节数
  uint64_t compaction_bytes = 0;    // 压缩写出的 SSTable 字节数
  uint64_t flushes = 0;
  uint64_t compactions = 0;         // 不含 trivial move
  uint64_t trivial_moves = 0;
  uint64_t block_reads = 0;         // 当前所有 SSTable 累计读数据块的次数
};

class DB {
 public:
  // 打开（不存在则创建）数据目录 dir，从 MANIFEST + WAL 恢复，并启动后台线程。失败抛 IOError。
  static std::unique_ptr<DB> Open(const Options& options, const std::string& dir);
  // 停止后台线程。内存表里的数据已在 WAL 中，不需要在关闭时刷盘。
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // 以下接口都是线程安全的
  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key);
  std::optional<std::string> Get(const std::string& key) const;

  // 范围扫描：返回 [start, end) 内所有存活的键值对（按键升序），最多 limit 条。
  // end 为空串表示没有上界。
  std::vector<std::pair<std::string, std::string>> Scan(
      const std::string& start, const std::string& end,
      size_t limit = std::numeric_limits<size_t>::max()) const;

  // 手动全量压缩：把每一层依次压到下一层，最终数据都在最底层（墓碑被彻底清除）。期间写入会等待。
  void CompactAll();
  // 等后台的刷盘/压缩全部做完（测试和基准用，让文件布局稳定下来）
  void WaitForIdle();

  // 调试/测试用：某一层当前的 SSTable 个数、所有 SSTable 的总字节数
  int NumFilesAtLevel(int level) const;
  uint64_t TotalTableBytes() const;
  DBStats GetStats() const;

 private:
  // 一次读操作看到的完整视图：三者在同一把锁下一起拷贝，保证互相一致
  struct SuperVersion {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;
    std::shared_ptr<const Version> version;
  };

  DB(const Options& options, const std::string& dir);

  // ---- 恢复（db.cpp）----
  void Recover();
  void LoadManifest(uint64_t* min_log_number);
  void ReplayLogs(uint64_t min_log_number);

  // ---- 读写（db.cpp）----
  void Write(ValueType type, const std::string& key, const std::string& value);
  void MakeRoomForWrite(std::unique_lock<std::mutex>& lock);
  void SwitchMemTable();
  SuperVersion GetSuperVersion() const;
  void ThrowIfBackgroundError() const;

  // ---- 后台工作（db_background.cpp）。带 lock 参数的函数：调用时持有 mu_，内部可能临时放锁 ----
  void BackgroundLoop();
  bool HasBackgroundWork() const;
  void DoBackgroundWork(std::unique_lock<std::mutex>& lock);
  void FlushImmutable(std::unique_lock<std::mutex>& lock);
  void BackgroundCompaction(std::unique_lock<std::mutex>& lock, const Compaction& c);
  void ManualCompaction(std::unique_lock<std::mutex>& lock);
  std::shared_ptr<FileMeta> WriteLevel0Table(const MemTable& mem, uint64_t number);
  void SaveManifest(const Version& version, uint64_t log_number);
  void RemoveObsoleteFiles();
  uint64_t MinLiveLogNumber() const { return imm_ ? imm_log_number_ : log_number_; }

  const Options options_;
  const std::string dir_;
  std::unique_ptr<FileLock> lock_;

  // 写者互斥锁：保证单写者，同时保护 wal_（只有持有它的写者会碰 wal_）
  std::mutex write_mu_;
  std::unique_ptr<WalWriter> wal_;

  // 保护以下所有成员
  mutable std::mutex mu_;
  std::condition_variable bg_cv_;  // 后台有活了 / 后台干完一件活了，都通过它通知
  std::shared_ptr<MemTable> mem_;  // 可写内存表
  std::shared_ptr<MemTable> imm_;  // 已冻结、等待刷盘的内存表（可能为空）
  uint64_t log_number_ = 0;        // mem_ 对应的 WAL 编号
  uint64_t imm_log_number_ = 0;    // imm_ 对应的（最老的）WAL 编号
  uint64_t next_file_number_ = 1;
  std::shared_ptr<const Version> current_;
  CompactPointers compact_pointers_;  // 只有后台线程使用
  bool shutting_down_ = false;
  bool manual_compaction_ = false;
  std::string bg_error_;  // 后台出错后记下原因；之后所有写入都会失败
  DBStats stats_;         // block_reads 字段不在这里维护，GetStats 时现算

  std::thread bg_thread_;  // 放在最后：构造时其它成员都已初始化
};

}  // namespace kv
