// db.h —— 对外接口
//
// 对应组件：LevelDB include/leveldb/db.h + db/db_impl.h。
// 面试可能问：
//   - Get 为什么返回 std::optional 而不是空字符串？
//     答：空字符串是合法的值，必须和"键不存在"区分开。
//   - 写入的顺序为什么是"先 WAL 后 MemTable"？反过来会怎样？
//     答：反过来的话，写完内存还没写日志时崩溃，这条数据可能已经被别的读者读到，
//     重启后却消失了（读到了"不存在的历史"）。先写日志保证：任何被看见过的写入都能恢复。
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "kv/file.h"
#include "kv/memtable.h"
#include "kv/options.h"
#include "kv/wal.h"

namespace kv {

class DB {
 public:
  // 打开（不存在则创建）数据目录 dir，并从 WAL 恢复数据。失败抛 IOError。
  static std::unique_ptr<DB> Open(const Options& options, const std::string& dir);

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key);
  std::optional<std::string> Get(const std::string& key) const;

 private:
  DB(const Options& options, const std::string& dir);
  void Recover();
  void Write(ValueType type, const std::string& key, const std::string& value);

  const Options options_;
  const std::string dir_;
  std::unique_ptr<FileLock> lock_;
  MemTable mem_;
  std::unique_ptr<WalWriter> wal_;
};

}  // namespace kv
