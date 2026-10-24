#include "kv/memtable.h"

#include <mutex>
#include <utility>
#include <vector>

namespace kv {

void MemTable::Put(const std::string& key, const std::string& value) {
  Add(key, ValueType::kValue, value);
}

void MemTable::Delete(const std::string& key) {
  Add(key, ValueType::kDeletion, std::string());
}

void MemTable::Add(const std::string& key, ValueType type, const std::string& value) {
  std::unique_lock<std::shared_mutex> lock(mu_);  // 写：独占
  auto old = table_.InsertOrAssign(key, Entry{type, value});
  if (old.has_value()) {
    // 覆盖写：键和节点已经算过了，只需要修正值的大小差
    memory_usage_ = memory_usage_ - old->value.size() + value.size();
  } else {
    memory_usage_ += key.size() + value.size() + kNodeOverhead;
  }
}

size_t MemTable::ApproximateMemoryUsage() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return memory_usage_;
}

size_t MemTable::Count() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return table_.Size();
}

LookupResult MemTable::Get(const std::string& key, std::string* value) const {
  std::shared_lock<std::shared_mutex> lock(mu_);  // 读：共享，多个读者可同时进入
  const Entry* entry = table_.Find(key);
  if (entry == nullptr) return LookupResult::kNotFound;
  if (entry->type == ValueType::kDeletion) return LookupResult::kDeleted;
  *value = entry->value;
  return LookupResult::kFound;
}

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  // [性能] 把整张表拷贝成快照再迭代：实现简单，并且迭代期间不需要一直持有锁
  // （Phase 6 并发时很重要）。代价是一次 O(n) 拷贝，内存最多翻倍到 2×memtable_size。
  // LevelDB 的做法是让迭代器直接走跳表（依赖无锁跳表的并发读安全）。
  std::shared_lock<std::shared_mutex> lock(mu_);
  std::vector<Record> records;
  records.reserve(table_.Size());
  SkipList<std::string, Entry>::Iterator it(&table_);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    records.push_back(Record{it.key(), it.value().type, it.value().value});
  }
  return std::make_unique<VectorIterator>(std::move(records));
}

}  // namespace kv
