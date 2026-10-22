#include "kv/version.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "kv/filename.h"

namespace kv {

const FileMeta* FindFileInLevel(const std::vector<std::shared_ptr<FileMeta>>& files,
                                const std::string& key) {
  // 第一个 largest >= key 的文件；同层不重叠，所以 key 只可能在这个文件里
  auto it = std::lower_bound(files.begin(), files.end(), key,
                             [](const auto& f, const std::string& k) { return f->largest() < k; });
  if (it == files.end() || key < (*it)->smallest()) return nullptr;
  return it->get();
}

LookupResult Version::Get(const std::string& key, std::string* value) const {
  // L0：文件之间键范围可能重叠，只能按 新→旧 逐个查
  for (const auto& file : levels[0]) {
    // 键不在 [smallest, largest] 范围内，这个文件一定没有它，连过滤器都不用查
    if (key < file->smallest() || key > file->largest()) continue;
    LookupResult r = file->table->Get(key, value);
    if (r != LookupResult::kNotFound) return r;
  }
  // L1+：每层最多查一个文件。层号越小数据越新，所以从上往下第一个有结论的就是答案
  for (int level = 1; level < kNumLevels; ++level) {
    const FileMeta* file = FindFileInLevel(levels[level], key);
    if (file == nullptr) continue;
    LookupResult r = file->table->Get(key, value);
    if (r != LookupResult::kNotFound) return r;
  }
  return LookupResult::kNotFound;
}

void Version::AddIterators(const std::string& start, const std::string& end,
                           std::vector<std::unique_ptr<Iterator>>* out) const {
  for (const auto& files : levels) {
    for (const auto& file : files) {
      // 范围裁剪：文件整体在扫描区间之外就不用打开迭代器
      if (file->largest() < start) continue;
      if (!end.empty() && file->smallest() >= end) continue;
      out->push_back(file->table->NewIterator());
    }
  }
}

void WriteManifest(const std::string& dir, const ManifestData& data) {
  std::ostringstream out;
  out << "next_file_number " << data.next_file_number << "\n";
  out << "log_number " << data.log_number << "\n";
  for (const auto& [level, number] : data.files) out << "file " << level << " " << number << "\n";
  WriteFileAtomically(ManifestFileName(dir), out.str());
}

std::optional<ManifestData> ReadManifest(const std::string& dir) {
  const std::string path = ManifestFileName(dir);
  if (!std::filesystem::exists(path)) return std::nullopt;

  ManifestData data;
  std::istringstream in(ReadFileToString(path));
  std::string tag;
  while (in >> tag) {
    if (tag == "next_file_number") {
      in >> data.next_file_number;
    } else if (tag == "log_number") {
      in >> data.log_number;
    } else if (tag == "file") {
      int level = -1;
      uint64_t number = 0;
      in >> level >> number;
      if (level < 0 || level >= kNumLevels) throw IOError("bad level in MANIFEST");
      data.files.emplace_back(level, number);
    } else {
      throw IOError("unknown tag in MANIFEST: " + tag);
    }
    if (in.fail()) throw IOError("malformed MANIFEST");
  }
  return data;
}

}  // namespace kv
