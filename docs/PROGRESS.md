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
- `DB::Open(options, dir)`：启动时重放 WAL，遇到残缺/损坏记录即停止（Phase 3 起改为重放后刷盘并换新 WAL，见 DECISIONS）。
- 测试：`tests/wal_test.cpp`（CRC 标准向量、往返、**在每一个字节位置截断**、比特翻转）、`tests/db_test.cpp`（重启恢复、截断 WAL 模拟崩溃后再写再重启、重复打开同一目录失败）。

已知问题：无。

## Phase 3：SSTable 与刷盘 ✅

- `include/kv/sstable_format.h`：块/索引/footer 编码，块级 crc 校验。
- `include/kv/sstable.h`：`TableBuilder`（顺序写，`Finish` 时 fsync）与 `Table`（索引常驻内存，`Get` 最多一次 pread；两级迭代器）。
- `include/kv/version.h`：不可变 `Version`（每层文件列表）+ 文本 MANIFEST（原子重写）。
- `DB`：MemTable 超过 `memtable_size` → 冻结为 `imm_` → 写 L0 SSTable → 更新 MANIFEST → 删除旧 WAL。此阶段刷盘在写线程同步完成。
- 测试：`tests/sstable_test.cpp`（点查/墓碑/缺失键、迭代与 Seek、块损坏检测、非 SSTable 文件）；`tests/db_test.cpp`（写入远超阈值后重启全部读回、新 SSTable 遮住旧值、孤儿 .sst 清理）。

已知问题：刷盘在写线程同步进行，写入会出现延迟毛刺（Phase 6 解决）。

## Phase 4：读路径与布隆过滤器 ✅

- `include/kv/bloom.h`：布隆过滤器（双重哈希），写入 SSTable 的过滤器块，打开表时载入内存；`Table::Get` 先查过滤器。
- `include/kv/version.h`：`Version::Get` 按 L0 新→旧、L1+ 每层二分一个文件的顺序查；`AddIterators` 按键范围裁剪文件。
- `include/kv/merging_iterator.h`：多路归并，同键只输出最新版本（含墓碑）。
- `DB::Scan(start, end, limit)`：MemTable + 冻结表 + 各 SSTable 归并，跳过墓碑。
- 测试：`tests/bloom_test.cpp`（无假阴性、误判率 < 2%）、`tests/merging_iterator_test.cpp`、`tests/db_fuzz_test.cpp`（**与 std::map 对拍**，含随机重启）。

运行更长的对拍：`KV_FUZZ_OPS=200000 ./build/kv_tests --gtest_filter='*Fuzz*'`

已知问题：无（L0 无限增长的问题已在 Phase 5 解决）。

## Phase 5：Compaction ✅

- `include/kv/compaction.h`：分层压缩。`PickCompaction` 按分数选层；`RunCompaction` 用归并迭代器合并、丢弃旧版本与可回收墓碑、按目标大小切分输出；`ApplyCompaction` 生成新 Version。
- `DB`：每次刷盘后循环压缩直到各层不超标；`CompactAll()` 手动全量压缩。
- 测试：`tests/compaction_test.cpp`——1000 个键覆盖写 20 轮后 SSTable 总量 < 3×活数据、文件数 < 15；删除全部键后 `CompactAll` 文件数归零；重启后各层有序；对拍测试改用很小的层容量，覆盖 L0~L3 的多层压缩（`KV_FUZZ_OPS=100000` 也通过）。

已知问题：刷盘和压缩仍在写线程同步执行，写延迟有明显毛刺（Phase 6 解决）。
