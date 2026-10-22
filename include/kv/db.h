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
#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kv/file.h"
#include "kv/memtable.h"
#include "kv/options.h"
#include "kv/version.h"
#include "kv/wal.h"

namespace kv {

class DB {
 public:
  // 打开（不存在则创建）数据目录 dir，并从 MANIFEST + WAL 恢复。失败抛 IOError。
  static std::unique_ptr<DB> Open(const Options& options, const std::string& dir);

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key);
  std::optional<std::string> Get(const std::string& key) const;

  // 范围扫描：返回 [start, end) 内所有存活的键值对（按键升序），最多 limit 条。
  // end 为空串表示没有上界。
  std::vector<std::pair<std::string, std::string>> Scan(
      const std::string& start, const std::string& end,
      size_t limit = std::numeric_limits<size_t>::max()) const;

  // 调试/测试用：某一层当前的 SSTable 个数
  int NumFilesAtLevel(int level) const;

 private:
  DB(const Options& options, const std::string& dir);

  // ---- 恢复 ----
  void Recover();
  void LoadManifest(uint64_t* min_log_number);
  void ReplayLogs(uint64_t min_log_number);

  // ---- 写路径 ----
  void Write(ValueType type, const std::string& key, const std::string& value);
  void MakeRoomForWrite();
  void SwitchMemTable();

  // ---- 刷盘 ----
  void FlushImmutable();
  std::shared_ptr<FileMeta> WriteLevel0Table(const MemTable& mem);
  void SaveManifest(const Version& version);
  void RemoveObsoleteFiles();
  uint64_t NewFileNumber() { return next_file_number_++; }

  const Options options_;
  const std::string dir_;
  std::unique_ptr<FileLock> lock_;

  std::shared_ptr<MemTable> mem_;  // 可写内存表
  std::shared_ptr<MemTable> imm_;  // 已冻结、等待刷盘的内存表（可能为空）
  std::unique_ptr<WalWriter> wal_;
  uint64_t log_number_ = 0;  // mem_ 对应的 WAL 编号
  uint64_t next_file_number_ = 1;
  std::shared_ptr<const Version> current_;
};

}  // namespace kv
