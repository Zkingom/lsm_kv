// db.h —— 对外接口
//
// 对应组件：LevelDB include/leveldb/db.h + db/db_impl.h。
// 面试可能问：
//   - Get 为什么返回 std::optional 而不是空字符串？
//     答：空字符串是合法的值，必须和"键不存在"区分开。
#pragma once

#include <optional>
#include <string>

#include "kv/memtable.h"

namespace kv {

// Phase 1：数据只在内存里
class DB {
 public:
  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key);
  std::optional<std::string> Get(const std::string& key) const;

 private:
  MemTable mem_;
};

}  // namespace kv
