// options.h —— 引擎可调参数
//
// 对应组件：LevelDB include/leveldb/options.h。
// 面试可能问：
//   - sync_wal 开和不开，分别能扛住什么样的故障？
//     答：不开时，write() 返回后数据在内核页缓存里——进程崩溃不丢，机器掉电可能丢最近的写。
//     开了之后每次写都 fdatasync，掉电也不丢，但写延迟从微秒级变成毫秒级。
#pragma once

#include <cstddef>

namespace kv {

struct Options {
  // [持久化][性能] 每次写入 WAL 后是否立刻 fdatasync。默认关闭，与 LevelDB 默认一致。
  bool sync_wal = false;
};

}  // namespace kv
