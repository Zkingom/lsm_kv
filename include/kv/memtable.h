// memtable.h —— 内存表
//
// 对应组件：LevelDB db/memtable.h。
// 面试可能问：
//   - MemTable 为什么必须有序？
//     答：刷盘时要直接顺序写成有序的 SSTable，不需要再排序；范围扫描也依赖有序。
//   - LevelDB 的 MemTable 里同一个键会存多个版本（带序列号），你这里为什么覆盖写？
//     答：LevelDB 需要支持快照读（读某个时间点的旧版本）。本项目不支持快照，
//     同一个键在 MemTable 里只保留最新值即可，结构更简单、也更省内存。
//     代价：没法实现快照/MVCC。见 docs/DECISIONS.md。
//   - 怎么判断 MemTable "满了"？
//     答：统计键值字节数 + 每个节点的固定开销，近似估计内存占用。
//   - 并发怎么保护？
//     答：内部一把 std::shared_mutex（读写锁）：Get / 迭代拿共享锁，可以多个读者同时进行；
//     Put / Delete 拿独占锁。写只有一个线程（DB 层保证），所以独占锁持有时间很短。
#pragma once

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>

#include "kv/iterator.h"
#include "kv/skiplist.h"
#include "kv/types.h"

namespace kv {

class MemTable {
 public:
  void Put(const std::string& key, const std::string& value);
  // 写入墓碑（即使这个键从没出现过也要写：它可能存在于更老的 SSTable 里）
  void Delete(const std::string& key);

  // 找到值时写入 *value 并返回 kFound；遇到墓碑返回 kDeleted；没有记录返回 kNotFound
  LookupResult Get(const std::string& key, std::string* value) const;

  // 近似内存占用（字节），用于判断是否需要刷盘
  size_t ApproximateMemoryUsage() const;
  size_t Count() const;

  // 返回按键升序的迭代器，墓碑也会出现在其中（type() == kDeletion）。
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  struct Entry {
    ValueType type = ValueType::kValue;
    std::string value;
  };

  void Add(const std::string& key, ValueType type, const std::string& value);

  // 每个跳表节点除了键值本身之外的固定开销估计（节点结构 + vector + 字符串头）
  static constexpr size_t kNodeOverhead = 96;

  mutable std::shared_mutex mu_;  // 保护下面两个成员
  SkipList<std::string, Entry> table_;
  size_t memory_usage_ = 0;
};

}  // namespace kv
