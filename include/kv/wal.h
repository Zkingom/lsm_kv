// wal.h —— 预写日志（Write-Ahead Log）
//
// 对应组件：LevelDB db/log_writer.h、db/log_reader.h。
// 磁盘格式（每条记录）：
//   [crc32: 4 字节][len: 4 字节][payload: len 字节]
//   payload = [type: 1][key_len: 4][key][value_len: 4][value]
//   crc32 覆盖 len 和 payload，任何一位损坏都能发现。
//
// 面试可能问：
//   - 为什么要先写日志再写内存表？
//     答：内存表断电即失。先把操作追加到日志，崩溃后重放日志就能恢复内存表。
//     追加写是顺序 I/O，比随机写数据页快得多——这是 LSM-tree 写快的根本原因之一。
//   - 写到一半崩溃，日志末尾会出现什么？怎么处理？
//     答：可能是半条记录（长度不够）或者内容是垃圾（校验和不对）。
//     重放时遇到第一条坏记录就停下，把它和之后的内容都丢掉（截断）。
//     被丢掉的那条写入本来就没有向客户端返回成功（或者没 fsync），所以不违反承诺。
//   - 为什么遇到坏记录不跳过、继续读后面的？
//     答：一条记录损坏后，后面记录的边界也不可信了（长度字段可能就是坏的）；
//     而且"只恢复前缀"保证了恢复出的状态是某个真实存在过的历史状态。
//   - LevelDB 把日志分成 32KB 的块，你为什么没有？
//     答：分块是为了坏记录后能重新同步到下一个块继续读。本项目坏记录直接截断，
//     不需要重新同步，格式可以简单很多。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "kv/file.h"
#include "kv/types.h"

namespace kv {

class WalWriter {
 public:
  // 以追加方式打开 path
  WalWriter(const std::string& path, bool sync_each_write);

  void AddRecord(ValueType type, std::string_view key, std::string_view value);
  void Sync() { file_.Sync(); }
  uint64_t Size() const { return file_.Size(); }

 private:
  WritableFile file_;
  bool sync_each_write_;
};

struct WalReplayResult {
  uint64_t valid_bytes = 0;  // 完好记录组成的前缀长度
  uint64_t file_bytes = 0;   // 文件实际长度；大于 valid_bytes 说明末尾有坏数据
  size_t records = 0;        // 成功重放的记录数
};

using WalApplyFn = std::function<void(ValueType, std::string_view key, std::string_view value)>;

// 从头读取日志，对每条完好的记录调用 apply；遇到第一条不完整/校验失败的记录就停止。
WalReplayResult ReplayWal(const std::string& path, const WalApplyFn& apply);

}  // namespace kv
