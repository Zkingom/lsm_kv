// version.h —— "当前有哪些 SSTable"的快照，以及它的持久化（MANIFEST）
//
// 对应组件：LevelDB db/version_set.h、db/version_edit.h。
//
// Version 是不可变对象：每次刷盘/Compaction 都生成一个新 Version 替换旧的
// （copy-on-write）。读者拿着旧 Version 的 shared_ptr 可以安心读完，不受影响。
//
// MANIFEST 是一个小文本文件，每次变更都整体重写（写临时文件 + rename）：
//   next_file_number 12
//   log_number 11
//   file 0 10
//   file 1 7
// 含义：下一个可用的文件编号；编号 >= log_number 的 WAL 还没刷盘需要重放；
// 每个 "file <层> <编号>" 是一个有效的 SSTable。
//
// 面试可能问：
//   - 为什么需要 MANIFEST？直接扫描目录里的 .sst 不行吗？
//     答：目录里可能有写了一半的 SSTable（刷盘中途崩溃），也可能有 Compaction 已经合并
//     但还没删除的旧文件。只有 MANIFEST 能说清"哪些文件是有效的、在第几层"。
//   - LevelDB 的 MANIFEST 是追加写 VersionEdit 的日志，你为什么整体重写？
//     答：整体重写 + rename 最简单，天然原子。代价是每次变更写整个文件列表——
//     文件数到上万时才会明显变慢，本项目规模下可以忽略。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kv/sstable.h"

namespace kv {

// 层数：L0 ~ L3。Compaction 策略见 docs/DECISIONS.md
constexpr int kNumLevels = 4;

struct FileMeta {
  uint64_t number = 0;
  std::shared_ptr<Table> table;  // 打开的表（持有 fd）

  const std::string& smallest() const { return table->smallest(); }
  const std::string& largest() const { return table->largest(); }
  uint64_t file_size() const { return table->file_size(); }
};

struct Version {
  // levels[0]：L0，按编号从新到旧排列，文件之间键范围可能重叠
  // levels[i>=1]：按 smallest 升序排列，同层文件键范围互不重叠
  std::array<std::vector<std::shared_ptr<FileMeta>>, kNumLevels> levels;

  // 在这个版本的 SSTable 里查 key（按 新→旧 的顺序，第一个有结论的为准）
  LookupResult Get(const std::string& key, std::string* value) const;
};

struct ManifestData {
  uint64_t next_file_number = 1;
  uint64_t log_number = 0;
  std::vector<std::pair<int, uint64_t>> files;  // (层, 文件编号)
};

// [持久化] 原子地整体重写 MANIFEST
void WriteManifest(const std::string& dir, const ManifestData& data);
// MANIFEST 不存在时返回 nullopt（新数据库）；格式错误抛 IOError
std::optional<ManifestData> ReadManifest(const std::string& dir);

}  // namespace kv
