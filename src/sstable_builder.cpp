#include "kv/sstable.h"

#include <cassert>

namespace kv {

TableBuilder::TableBuilder(const std::string& path, const Options& options)
    : block_size_(options.block_size), file_(path, /*truncate=*/true) {}

void TableBuilder::Add(std::string_view key, ValueType type, std::string_view value) {
  assert(num_entries_ == 0 || key > last_key_);  // 调用方保证有序
  AppendDataEntry(&data_block_, key, type, value);
  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  // [性能] 块越大：索引越小、顺序扫描越快；但随机读一个键要读入/解析的数据越多。
  if (data_block_.size() >= block_size_) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return;
  BlockHandle handle = WriteBlock(&file_, data_block_);
  AppendIndexEntry(&index_block_, IndexEntry{last_key_, handle});
  data_block_.clear();
}

uint64_t TableBuilder::Finish() {
  FlushDataBlock();
  Footer footer;
  footer.filter = BlockHandle{file_.Size(), 0};  // Phase 3 还没有过滤器
  footer.index = WriteBlock(&file_, index_block_);
  file_.Append(EncodeFooter(footer));
  // [持久化] 必须 fsync 之后才能把这个文件记进 MANIFEST，否则掉电后
  // MANIFEST 指向一个内容不完整的文件。
  file_.Sync();
  return file_.Size();
}

}  // namespace kv
