// compaction.h —— 分层压缩（Leveled Compaction）：选文件 + 合并
//
// 对应组件：LevelDB db/version_set.cc（PickCompaction）+ db/db_impl.cc（DoCompactionWork）。
//
// 本项目的策略（详见 docs/DECISIONS.md）：
//   - 共 4 层 L0~L3。L0 文件之间可以重叠；L1~L3 同层内不重叠。
//   - 触发条件（算一个"分数"，分数 >= 1 就需要压缩，挑分数最高的层）：
//       L0：文件个数 / l0_compaction_trigger（默认 4 个）
//       Li：该层总字节数 / (level1_max_bytes × 10^(i-1))（L1=10MB, L2=100MB）
//       L3 是最底层，不再往下压。
//   - L0 → L1：取 L0 全部文件 + L1 中与它们键范围重叠的文件。
//   - Li → Li+1：在 Li 里轮流（round-robin）挑一个文件 + Li+1 中重叠的文件。
//   - 只有一个输入文件、且下一层没有重叠时，直接把文件"挪"到下一层（trivial move），不重写。
//
// 面试可能问：
//   - 为什么要 Compaction？
//     答：① 回收空间：被覆盖的旧版本、被删除的数据只有在合并时才真正丢掉（空间放大）；
//     ② 控制读放大：L0 文件互相重叠，个数越多，一次读要查的文件越多。
//   - 墓碑什么时候能丢掉？
//     答：只有确定更深的层里没有这个键的旧版本时。否则丢掉墓碑，旧值就"复活"了。
//     本项目检查"输出层之下的所有层，有没有文件的键范围覆盖这个键"。
//   - Leveled 和 Tiered（Size-tiered）有什么区别？
//     答：Leveled 每层只有一个有序"run"，读放大和空间放大小，但同一条数据会被反复重写
//     （写放大约 层数×10）；Tiered 每层允许多个 run，攒够了一起合并，写放大小但读/空间放大大。
//     RocksDB 默认 Leveled，Cassandra 默认 Size-tiered。
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kv/options.h"
#include "kv/version.h"

namespace kv {

struct Compaction {
  int level = 0;  // 输入来自 level 和 level+1，输出写到 level+1
  std::vector<std::shared_ptr<FileMeta>> inputs;       // 来自 level
  std::vector<std::shared_ptr<FileMeta>> next_inputs;  // 来自 level+1，与 inputs 键范围重叠

  bool IsTrivialMove() const { return inputs.size() == 1 && next_inputs.empty(); }
};

// 每层下一次从哪个键之后开始挑文件（round-robin），保证一层里的文件轮流被压缩
using CompactPointers = std::array<std::string, kNumLevels>;

uint64_t MaxBytesForLevel(const Options& options, int level);

// 根据分数挑一个需要做的压缩；都不需要时返回 nullopt
std::optional<Compaction> PickCompaction(const Version& version, const Options& options,
                                         CompactPointers* pointers);

// 构造"把 level 层全部文件压到 level+1"的压缩（手动全量压缩用）；该层为空返回 nullopt
std::optional<Compaction> CompactWholeLevel(const Version& version, int level);

// 执行合并，写出新 SSTable，返回输出文件（尚未加入任何 Version）。
// new_file_number 由调用方提供（以便调用方加锁或记录"正在生成的文件"）。
std::vector<std::shared_ptr<FileMeta>> RunCompaction(
    const Compaction& c, const Version& version, const Options& options, const std::string& dir,
    const std::function<uint64_t()>& new_file_number);

// 基于 base 生成新版本：删除 c 的所有输入，把 outputs 加到 level+1
std::shared_ptr<Version> ApplyCompaction(const Version& base, const Compaction& c,
                                         const std::vector<std::shared_ptr<FileMeta>>& outputs);

}  // namespace kv
