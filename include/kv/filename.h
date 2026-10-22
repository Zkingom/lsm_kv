// filename.h —— 数据目录里各类文件的命名规则
//
// 对应组件：LevelDB db/filename.h。
// 目录布局：
//   LOCK            进程锁
//   000001.log      WAL（编号单调递增）
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace kv {

inline std::string NumberedFileName(const std::string& dir, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dir + buf;
}

inline std::string LogFileName(const std::string& dir, uint64_t number) {
  return NumberedFileName(dir, number, "log");
}

inline std::string LockFileName(const std::string& dir) { return dir + "/LOCK"; }

}  // namespace kv
