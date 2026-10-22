#include "kv/version.h"

#include <filesystem>
#include <sstream>

#include "kv/filename.h"

namespace kv {

LookupResult Version::Get(const std::string& key, std::string* value) const {
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& file : levels[level]) {
      // 键不在 [smallest, largest] 范围内，这个文件一定没有它，连索引都不用查
      if (key < file->smallest() || key > file->largest()) continue;
      LookupResult r = file->table->Get(key, value);
      if (r != LookupResult::kNotFound) return r;
    }
  }
  return LookupResult::kNotFound;
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
