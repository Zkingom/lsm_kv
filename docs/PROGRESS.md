# 进度

## 怎么运行

需要 Linux（或 WSL）、g++ ≥ 11、CMake ≥ 3.20。

```bash
scripts/build_and_test.sh            # 编译到 ./build 并跑全部测试
```

## Phase 1：MemTable ✅

- `include/kv/skiplist.h`：模板跳表，支持插入/覆盖、查找、有序迭代与 Seek。
- `include/kv/memtable.h`：基于跳表的内存表，删除写墓碑，`Get` 返回三态（找到/已删除/不知道）。
- `include/kv/db.h`：`DB::Put / Get / Delete`，此阶段只在内存。
- 测试：`tests/skiplist_test.cpp`（与 `std::map` 对比有序性和 Seek）、`tests/memtable_test.cpp`（覆盖写、墓碑、删除不存在的键、有序遍历、内存统计）、`tests/db_test.cpp`。

已知问题：无。
