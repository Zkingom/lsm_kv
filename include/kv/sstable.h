// sstable.h —— SSTable 的写入（TableBuilder）与只读访问（Table）
//
// 对应组件：LevelDB table/table_builder.h、include/leveldb/table.h。
// 格式见 sstable_format.h。
//
// 面试可能问：
//   - SSTable 为什么是只读（不可变）的？好处是什么？
//     答：① 写入只有顺序追加，没有随机写；② 多线程读不需要任何锁；
//     ③ 崩溃时不存在"改了一半"的文件——要么完整写完被 MANIFEST 引用，要么是孤儿文件直接删。
//     代价是更新/删除只能写新记录，旧数据靠 Compaction 回收（空间放大）。
//   - 查一个键需要几次磁盘 I/O？
//     答：索引和过滤器在打开文件时就读进内存，所以每次查找最多读 1 个数据块（1 次 pread）。
//     加了布隆过滤器后，对不存在的键大概率 0 次 I/O。
//   - 为什么 Table 继承 enable_shared_from_this？
//     答：迭代器需要持有 Table 的 shared_ptr，保证迭代过程中即使 Compaction 把这个表
//     从版本里移除了，表对象（和打开的 fd）也不会被提前销毁。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kv/file.h"
#include "kv/iterator.h"
#include "kv/options.h"
#include "kv/sstable_format.h"
#include "kv/types.h"

namespace kv {

class TableBuilder {
 public:
  TableBuilder(const std::string& path, const Options& options);

  // 键必须严格递增。墓碑也要写进去：它要遮住更老文件里的旧值。
  void Add(std::string_view key, ValueType type, std::string_view value);
  // 写出剩余数据块、过滤器、索引和 footer，并 fsync。返回文件大小。
  uint64_t Finish();

  uint64_t NumEntries() const { return num_entries_; }
  // 当前已写出的字节数 + 缓冲中的字节数（Compaction 用它决定何时切换到新文件）
  uint64_t EstimatedSize() const { return file_.Size() + data_block_.size(); }

 private:
  void FlushDataBlock();

  const size_t block_size_;
  const bool use_bloom_;
  const int bloom_bits_per_key_;
  WritableFile file_;
  std::string data_block_;   // 正在攒的数据块
  std::string index_block_;  // 所有已写出数据块的索引
  std::string last_key_;
  uint64_t num_entries_ = 0;
  std::vector<uint64_t> key_hashes_;  // 用于最后构造布隆过滤器（只存 8 字节哈希，不存键）
};

class Table : public std::enable_shared_from_this<Table> {
 public:
  // 打开并读入 footer、索引；失败抛 IOError
  static std::shared_ptr<Table> Open(const std::string& path);

  // 布隆过滤器判断：false 表示一定不存在
  bool MayContain(const std::string& key) const;
  // 语义同 MemTable::Get：kFound / kDeleted（墓碑）/ kNotFound。会先查布隆过滤器
  LookupResult Get(const std::string& key, std::string* value) const;
  // 有序迭代整个表（包括墓碑）
  std::unique_ptr<Iterator> NewIterator() const;

  const std::string& smallest() const { return smallest_; }
  const std::string& largest() const { return index_.back().last_key; }
  uint64_t file_size() const { return file_.Size(); }

 private:
  friend class TableIterator;
  explicit Table(const std::string& path) : file_(path) {}

  // 第一个 last_key >= key 的数据块下标；所有块都比 key 小时返回 index_.size()
  size_t FindBlock(const std::string& key) const;
  std::vector<Record> ReadDataBlock(size_t block_index) const;

  RandomAccessFile file_;
  std::vector<IndexEntry> index_;
  std::string filter_;  // 布隆过滤器，常驻内存；为空表示没有过滤器
  std::string smallest_;
};

}  // namespace kv
