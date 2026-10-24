#include "kv/compaction.h"

#include <algorithm>

#include "kv/filename.h"
#include "kv/merging_iterator.h"

namespace kv {
namespace {

using FileList = std::vector<std::shared_ptr<FileMeta>>;

uint64_t TotalBytes(const FileList& files) {
  uint64_t total = 0;
  for (const auto& f : files) total += f->file_size();
  return total;
}

// 一组文件覆盖的整体键范围 [smallest, largest]
void KeyRange(const FileList& files, std::string* smallest, std::string* largest) {
  *smallest = files.front()->smallest();
  *largest = files.front()->largest();
  for (const auto& f : files) {
    if (f->smallest() < *smallest) *smallest = f->smallest();
    if (f->largest() > *largest) *largest = f->largest();
  }
}

FileList OverlappingFiles(const FileList& files, const std::string& smallest,
                          const std::string& largest) {
  FileList result;
  for (const auto& f : files) {
    if (f->largest() >= smallest && f->smallest() <= largest) result.push_back(f);
  }
  return result;
}

// 输出层之下的所有层里，都没有文件的键范围覆盖 key：说明 key 没有更老的版本了
bool IsBaseLevelForKey(const Version& version, int output_level, const std::string& key) {
  for (int level = output_level + 1; level < kNumLevels; ++level) {
    for (const auto& f : version.levels[level]) {
      if (key >= f->smallest() && key <= f->largest()) return false;
    }
  }
  return true;
}

// 每层的"分数"：>= 1 表示该层超标需要压缩
double LevelScore(const Version& version, const Options& options, int level) {
  if (level == 0) {
    // L0 按文件个数而不是字节数：L0 文件互相重叠，读一次要查每个文件，个数才是读放大的来源
    return static_cast<double>(version.levels[0].size()) / options.l0_compaction_trigger;
  }
  return static_cast<double>(TotalBytes(version.levels[level])) /
         static_cast<double>(MaxBytesForLevel(options, level));
}

// 在 level 层（level >= 1）挑一个文件：compact pointer 之后的第一个，到末尾就绕回开头
std::shared_ptr<FileMeta> PickFileRoundRobin(const FileList& files, std::string* pointer) {
  std::shared_ptr<FileMeta> chosen = files.front();
  for (const auto& f : files) {
    if (f->smallest() > *pointer) {
      chosen = f;
      break;
    }
  }
  *pointer = chosen->largest();
  return chosen;
}

std::shared_ptr<FileMeta> FinishOutput(TableBuilder* builder, const std::string& dir,
                                       uint64_t number) {
  builder->Finish();
  auto meta = std::make_shared<FileMeta>();
  meta->number = number;
  meta->table = Table::Open(TableFileName(dir, number));
  return meta;
}

}  // namespace

uint64_t MaxBytesForLevel(const Options& options, int level) {
  uint64_t bytes = options.level1_max_bytes;
  for (int i = 1; i < level; ++i) bytes *= 10;  // 每层是上一层的 10 倍
  return bytes;
}

bool NeedsCompaction(const Version& version, const Options& options) {
  for (int level = 0; level < kNumLevels - 1; ++level) {
    if (LevelScore(version, options, level) >= 1.0) return true;
  }
  return false;
}

std::optional<Compaction> PickCompaction(const Version& version, const Options& options,
                                         CompactPointers* pointers) {
  int best_level = -1;
  double best_score = 0;
  // 最底层（kNumLevels-1）不再往下压，所以不参与评分
  for (int level = 0; level < kNumLevels - 1; ++level) {
    double score = LevelScore(version, options, level);
    if (score >= 1.0 && score > best_score) {
      best_level = level;
      best_score = score;
    }
  }
  if (best_level < 0) return std::nullopt;

  Compaction c;
  c.level = best_level;
  if (best_level == 0) {
    // L0 文件互相重叠，只挑其中一个的话，留下的更老文件可能比输出到 L1 的数据还旧，
    // 破坏"层越深越旧"的规则。最简单的做法是一次全拿。
    c.inputs = version.levels[0];
  } else {
    c.inputs.push_back(PickFileRoundRobin(version.levels[best_level], &(*pointers)[best_level]));
  }
  std::string smallest, largest;
  KeyRange(c.inputs, &smallest, &largest);
  c.next_inputs = OverlappingFiles(version.levels[best_level + 1], smallest, largest);
  return c;
}

std::optional<Compaction> CompactWholeLevel(const Version& version, int level) {
  if (version.levels[level].empty()) return std::nullopt;
  Compaction c;
  c.level = level;
  c.inputs = version.levels[level];
  std::string smallest, largest;
  KeyRange(c.inputs, &smallest, &largest);
  c.next_inputs = OverlappingFiles(version.levels[level + 1], smallest, largest);
  return c;
}

std::vector<std::shared_ptr<FileMeta>> RunCompaction(
    const Compaction& c, const Version& version, const Options& options, const std::string& dir,
    const std::function<uint64_t()>& new_file_number) {
  // 子迭代器按 新→旧：level 层的输入比 level+1 层的新（L0 的 inputs 本身已是新→旧）
  std::vector<std::unique_ptr<Iterator>> children;
  for (const auto& f : c.inputs) children.push_back(f->table->NewIterator());
  for (const auto& f : c.next_inputs) children.push_back(f->table->NewIterator());
  MergingIterator it(std::move(children));

  const int output_level = c.level + 1;
  std::vector<std::shared_ptr<FileMeta>> outputs;
  std::unique_ptr<TableBuilder> builder;
  uint64_t number = 0;
  // 归并迭代器对每个键只给出最新版本：被覆盖的旧版本在这里自然就被丢掉了
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    if (it.type() == ValueType::kDeletion && IsBaseLevelForKey(version, output_level, it.key())) {
      continue;  // 下面没有旧版本了，墓碑完成了使命，可以真正删除
    }
    if (!builder) {
      number = new_file_number();
      builder = std::make_unique<TableBuilder>(TableFileName(dir, number), options);
    }
    builder->Add(it.key(), it.type(), it.value());
    // [性能] 输出切成多个 ~target_file_size 的文件：下次压缩时只需重写重叠的那几个小文件
    if (builder->EstimatedSize() >= options.target_file_size) {
      outputs.push_back(FinishOutput(builder.get(), dir, number));
      builder.reset();
    }
  }
  if (builder) outputs.push_back(FinishOutput(builder.get(), dir, number));
  return outputs;
}

std::shared_ptr<Version> ApplyCompaction(const Version& base, const Compaction& c,
                                         const std::vector<std::shared_ptr<FileMeta>>& outputs) {
  auto version = std::make_shared<Version>(base);  // copy-on-write
  auto remove_all = [](FileList* files, const FileList& dead) {
    std::erase_if(*files, [&](const auto& f) {
      return std::find(dead.begin(), dead.end(), f) != dead.end();
    });
  };
  remove_all(&version->levels[c.level], c.inputs);
  remove_all(&version->levels[c.level + 1], c.next_inputs);

  FileList& next = version->levels[c.level + 1];
  next.insert(next.end(), outputs.begin(), outputs.end());
  std::sort(next.begin(), next.end(),
            [](const auto& a, const auto& b) { return a->smallest() < b->smallest(); });
  return version;
}

}  // namespace kv
