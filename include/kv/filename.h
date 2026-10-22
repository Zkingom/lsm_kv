// filename.h —— 数据目录里各类文件的命名规则
//
// 对应组件：LevelDB db/filename.h。
// 目录布局：
//   LOCK            进程锁
//   MANIFEST        当前有哪些 SSTable、各在哪一层（见 version.h）
//   000005.log      WAL
//   000007.sst      SSTable
// WAL 和 SSTable 共用一个单调递增的文件编号计数器，编号越大越新。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kv {

enum class FileType { kLog, kTable, kOther };

inline std::string NumberedFileName(const std::string& dir, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dir + buf;
}

inline std::string LogFileName(const std::string& dir, uint64_t number) {
  return NumberedFileName(dir, number, "log");
}

inline std::string TableFileName(const std::string& dir, uint64_t number) {
  return NumberedFileName(dir, number, "sst");
}

inline std::string LockFileName(const std::string& dir) { return dir + "/LOCK"; }
inline std::string ManifestFileName(const std::string& dir) { return dir + "/MANIFEST"; }

// 解析 "000123.log" / "000123.sst" 这样的文件名（不含目录）
inline FileType ParseFileName(const std::string& name, uint64_t* number) {
  size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0) return FileType::kOther;
  for (size_t i = 0; i < dot; ++i) {
    if (name[i] < '0' || name[i] > '9') return FileType::kOther;
  }
  std::string suffix = name.substr(dot + 1);
  FileType type = suffix == "log" ? FileType::kLog : suffix == "sst" ? FileType::kTable : FileType::kOther;
  if (type != FileType::kOther) *number = std::strtoull(name.substr(0, dot).c_str(), nullptr, 10);
  return type;
}

}  // namespace kv
