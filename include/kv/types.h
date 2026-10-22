// types.h —— 全引擎共用的小类型
//
// 对应组件：LevelDB db/dbformat.h 里的 ValueType。
// 面试可能问：
//   - 为什么删除不能直接把键从内存表里抹掉，而要写一个"墓碑"？
//     答：旧版本可能还躺在更老的 SSTable 里。如果只在内存里删掉，读的时候往下查到
//     旧文件就会把已删除的值"复活"。墓碑是一条"更新的记录"，读路径遇到它就停。
#pragma once

#include <cstdint>

namespace kv {

// 一条记录的类型。数值会写进 WAL 和 SSTable，所以一旦确定不能再改。
enum class ValueType : uint8_t {
  kDeletion = 0,  // 墓碑
  kValue = 1,
};

// 在某一层（MemTable / 某个 SSTable）里查一个键的结果。
// 需要三态而不是 bool：kDeleted 表示"确定被删了，不要再往更老的数据里找"，
// 而 kNotFound 表示"这一层不知道，继续往下查"。
enum class LookupResult {
  kFound,
  kDeleted,
  kNotFound,
};

}  // namespace kv
