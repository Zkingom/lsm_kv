#include "kv/sstable.h"

#include <algorithm>

#include "kv/bloom.h"

namespace kv {

// ---------------- Table ----------------

std::shared_ptr<Table> Table::Open(const std::string& path) {
  // 构造函数私有，不能用 make_shared
  std::shared_ptr<Table> table(new Table(path));
  const RandomAccessFile& file = table->file_;
  if (file.Size() < kFooterSize) throw IOError("sstable too small: " + path);

  Footer footer = DecodeFooter(file.Read(file.Size() - kFooterSize, kFooterSize));
  table->index_ = DecodeIndexBlock(ReadBlock(file, footer.index));
  if (footer.filter.size > 0) table->filter_ = ReadBlock(file, footer.filter);
  if (table->index_.empty()) throw IOError("sstable has no data: " + path);

  // 最小键 = 第一个数据块的第一个键。打开时读一次，后面 Compaction 判断键范围重叠要用。
  std::vector<Record> first_block = table->ReadDataBlock(0);
  if (first_block.empty()) throw IOError("sstable first block is empty: " + path);
  table->smallest_ = first_block.front().key;
  return table;
}

size_t Table::FindBlock(const std::string& key) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), key,
                             [](const IndexEntry& e, const std::string& k) { return e.last_key < k; });
  return static_cast<size_t>(it - index_.begin());
}

std::vector<Record> Table::ReadDataBlock(size_t block_index) const {
  // [性能] 每次都从文件读并完整解析一个块，没有块缓存（Block Cache）。
  // 热点数据会被重复读——不过操作系统页缓存会兜底，实际多数是内存拷贝而非磁盘 I/O。
  block_reads_.fetch_add(1, std::memory_order_relaxed);
  return DecodeDataBlock(ReadBlock(file_, index_[block_index].handle));
}

bool Table::MayContain(const std::string& key) const { return BloomMayContain(filter_, key); }

LookupResult Table::Get(const std::string& key, std::string* value) const {
  // 过滤器说"一定没有"，就不必读数据块了——对不存在的键省掉一次磁盘读
  if (!MayContain(key)) return LookupResult::kNotFound;
  size_t block = FindBlock(key);
  if (block == index_.size()) return LookupResult::kNotFound;

  std::vector<Record> records = ReadDataBlock(block);
  auto it = std::lower_bound(records.begin(), records.end(), key,
                             [](const Record& r, const std::string& k) { return r.key < k; });
  if (it == records.end() || it->key != key) return LookupResult::kNotFound;
  if (it->type == ValueType::kDeletion) return LookupResult::kDeleted;
  *value = it->value;
  return LookupResult::kFound;
}

// ---------------- TableIterator ----------------

// 两级迭代：外层走索引（第几个块），内层走当前块解析出来的记录。
class TableIterator : public Iterator {
 public:
  explicit TableIterator(std::shared_ptr<const Table> table) : table_(std::move(table)) {}

  bool Valid() const override { return block_ < table_->index_.size() && pos_ < records_.size(); }

  void SeekToFirst() override {
    LoadBlock(0);
    pos_ = 0;
  }

  void Seek(const std::string& target) override {
    LoadBlock(table_->FindBlock(target));
    auto it = std::lower_bound(records_.begin(), records_.end(), target,
                               [](const Record& r, const std::string& t) { return r.key < t; });
    pos_ = static_cast<size_t>(it - records_.begin());
  }

  void Next() override {
    if (++pos_ < records_.size()) return;
    LoadBlock(block_ + 1);  // 当前块读完，换下一块
    pos_ = 0;
  }

  const std::string& key() const override { return records_[pos_].key; }
  const std::string& value() const override { return records_[pos_].value; }
  ValueType type() const override { return records_[pos_].type; }

 private:
  void LoadBlock(size_t block) {
    block_ = block;
    records_.clear();
    if (block_ < table_->index_.size()) records_ = table_->ReadDataBlock(block_);
  }

  std::shared_ptr<const Table> table_;  // 持有所有权：迭代期间表不会被销毁
  size_t block_ = 0;
  size_t pos_ = 0;
  std::vector<Record> records_;
};

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<TableIterator>(shared_from_this());
}

}  // namespace kv
