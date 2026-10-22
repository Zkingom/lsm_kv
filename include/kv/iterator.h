// iterator.h —— 统一的有序迭代器接口
//
// 对应组件：LevelDB include/leveldb/iterator.h。
// 面试可能问：
//   - 为什么 MemTable、SSTable、归并结果都要实现同一个迭代器接口？
//     答：这样范围扫描可以把它们统一当作"若干有序流"做多路归并，
//     上层不关心数据在内存还是磁盘。这是 LSM-tree 读路径的核心抽象。
#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "kv/types.h"

namespace kv {

class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // 定位到第一个 key >= target 的位置
  virtual void Seek(const std::string& target) = 0;
  virtual void Next() = 0;

  // 以下三个只能在 Valid() 为 true 时调用
  virtual const std::string& key() const = 0;
  virtual const std::string& value() const = 0;
  virtual ValueType type() const = 0;
};

// 一条完整记录：键、类型、值。墓碑的 value 为空。
struct Record {
  std::string key;
  ValueType type;
  std::string value;
};

// 基于已排序 vector 的迭代器。
// MemTable 用它做"快照迭代"（见 memtable.h），测试里也可以直接拿来构造输入。
class VectorIterator : public Iterator {
 public:
  // records 必须已按 key 升序且无重复
  explicit VectorIterator(std::vector<Record> records) : records_(std::move(records)) {}

  bool Valid() const override { return pos_ < records_.size(); }
  void SeekToFirst() override { pos_ = 0; }
  void Seek(const std::string& target) override {
    auto it = std::lower_bound(
        records_.begin(), records_.end(), target,
        [](const Record& r, const std::string& t) { return r.key < t; });
    pos_ = static_cast<size_t>(it - records_.begin());
  }
  void Next() override { ++pos_; }
  const std::string& key() const override { return records_[pos_].key; }
  const std::string& value() const override { return records_[pos_].value; }
  ValueType type() const override { return records_[pos_].type; }

 private:
  std::vector<Record> records_;
  size_t pos_ = 0;
};

}  // namespace kv
