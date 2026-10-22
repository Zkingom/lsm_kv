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

## Phase 2：WAL 与崩溃恢复 ✅

- `include/kv/crc32.h`：查表法 CRC32（IEEE），编译期生成表。
- `include/kv/coding.h`：小端定长整数编解码。
- `include/kv/file.h`：POSIX 文件封装（追加写、pread 随机读、flock 目录锁、截断、fsync 目录、原子替换）。
- `include/kv/wal.h`：WAL 写入与重放；重放遇到不完整/校验失败记录即停止，返回有效前缀长度。
- `DB::Open(options, dir)`：启动时重放 `000001.log`，末尾坏数据物理截断后继续追加。
- 测试：`tests/wal_test.cpp`（CRC 标准向量、往返、**在每一个字节位置截断**、比特翻转）、`tests/db_test.cpp`（重启恢复、截断 WAL 模拟崩溃后再写再重启、重复打开同一目录失败）。

已知问题：无。
