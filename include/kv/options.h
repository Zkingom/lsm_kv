// options.h —— 引擎可调参数
//
// 对应组件：LevelDB include/leveldb/options.h。
// 面试可能问：
//   - sync_wal 开和不开，分别能扛住什么样的故障？
//     答：不开时，write() 返回后数据在内核页缓存里——进程崩溃不丢，机器掉电可能丢最近的写。
//     开了之后每次写都 fdatasync，掉电也不丢，但写延迟从微秒级变成毫秒级。
//   - MemTable 阈值调大/调小各有什么代价？
//     答：调大：刷盘次数少、L0 文件少、写放大略低；代价是内存占用高、崩溃后重放 WAL 更久。
//     调小：反之，并且 L0 文件多会让读变慢、更频繁触发 Compaction。
#pragma once

#include <cstddef>
#include <cstdint>

namespace kv {

struct Options {
  // [持久化][性能] 每次写入 WAL 后是否立刻 fdatasync。默认关闭，与 LevelDB 默认一致。
  bool sync_wal = false;

  // MemTable 近似内存占用超过该值就冻结并刷成 SSTable
  size_t memtable_size = 4 << 20;

  // SSTable 数据块的目标大小
  size_t block_size = 4 << 10;

  // 每个 SSTable 是否带布隆过滤器，以及每个键占多少比特（10 比特 ≈ 0.8% 误判率）
  // [性能] 代价：每个键多占 bits/8 字节内存（过滤器常驻内存），写 SSTable 时多算一次哈希。
  bool use_bloom_filter = true;
  int bloom_bits_per_key = 10;

  // ---- Compaction（策略见 compaction.h 和 docs/DECISIONS.md）----
  // L0 文件数达到该值触发 L0→L1 压缩
  int l0_compaction_trigger = 4;
  // L1 的容量上限；L2、L3 依次 ×10
  uint64_t level1_max_bytes = 10 << 20;
  // 压缩输出的单个 SSTable 目标大小
  uint64_t target_file_size = 2 << 20;
};

}  // namespace kv
