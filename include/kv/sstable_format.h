// sstable_format.h —— SSTable 的磁盘格式（读写两端共用）
//
// 对应组件：LevelDB table/format.h、table/block_builder.h。
//
// 文件布局：
//   [数据块 0][crc]
//   [数据块 1][crc]
//   ...
//   [过滤器块][crc]      ← 布隆过滤器（Phase 4），没有时长度为 0
//   [索引块][crc]
//   [footer: 40 字节]    ← 固定长度，放在文件最末尾，打开文件时先读它
//
// 数据块里的每条记录：[type: 1][key_len: 4][key][value_len: 4][value]
// 索引块里的每条记录：[key_len: 4][该数据块最后一个键][offset: 8][size: 8]
// footer：[index offset: 8][index size: 8][filter offset: 8][filter size: 8][magic: 8]
//
// 面试可能问：
//   - footer 为什么放在文件末尾、而且定长？
//     答：写文件时索引要等所有数据块写完才知道内容，只能放后面；
//     定长则让读者不需要任何先验信息：从 文件大小-40 处读就能找到索引的位置。
//   - 索引里为什么存"块的最后一个键"？
//     答：查键 k 时，二分找第一个 last_key >= k 的块，k 如果存在就只可能在这一块。
//   - 为什么每个块单独带 crc，而不是整个文件一个？
//     答：读一个键只读一个块，只需要校验这一块；整文件 crc 要读完整个文件才能验证。
//   - magic number 有什么用？
//     答：快速识别"这根本不是一个 SSTable"（比如文件被截断、或者传错了路径）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kv/file.h"
#include "kv/iterator.h"
#include "kv/types.h"

namespace kv {

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // 块内容长度，不含后面 4 字节的 crc
};

struct Footer {
  BlockHandle index;
  BlockHandle filter;
};

struct IndexEntry {
  std::string last_key;  // 该数据块中最大的键
  BlockHandle handle;
};

constexpr size_t kFooterSize = 40;
constexpr uint64_t kTableMagic = 0x314c42545353564bull;  // 小端字节就是 "KVSSTBL1"
constexpr size_t kBlockTrailerSize = 4;                   // 每个块后面的 crc32

std::string EncodeFooter(const Footer& footer);
// 长度或 magic 不对时抛 IOError
Footer DecodeFooter(std::string_view data);

void AppendDataEntry(std::string* block, std::string_view key, ValueType type,
                     std::string_view value);
// 解析整个数据块，格式错误抛 IOError
std::vector<Record> DecodeDataBlock(std::string_view contents);

void AppendIndexEntry(std::string* block, const IndexEntry& entry);
std::vector<IndexEntry> DecodeIndexBlock(std::string_view contents);

// [持久化] 写出块内容 + crc，返回块的位置
BlockHandle WriteBlock(WritableFile* file, std::string_view contents);
// 读出块内容并校验 crc，不一致抛 IOError
std::string ReadBlock(const RandomAccessFile& file, const BlockHandle& handle);

}  // namespace kv
