# 进度

## 怎么运行

需要 Linux（或 WSL）、g++ ≥ 11、CMake ≥ 3.20。

```bash
scripts/build_and_test.sh            # 编译到 ./build 并跑全部测试
scripts/run_tsan.sh                  # ThreadSanitizer 编译并跑全部测试
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

已知问题：无（Phase 6 起刷盘在后台线程）。

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

已知问题：无（同步刷盘/压缩的问题已在 Phase 6 解决）。

## Phase 6：并发 ✅

- 单写者（`write_mu_`）+ 多读者（锁内只拷贝 SuperVersion 指针，锁外读不可变数据）+ 一个后台线程（刷盘优先，其次压缩）。
- `src/db_background.cpp`：后台循环、刷盘、自动/手动压缩、MANIFEST 与过期文件清理；耗时 I/O 都在放锁后进行。
- 写停顿：冻结表未刷完、或 L0 ≥ 12 个文件时写者等待。后台错误会让后续写入抛异常。
- 新接口：`DB::WaitForIdle()`（等后台空闲）。
- 测试：`tests/concurrency_test.cpp`——1 写 4 读，读者校验"读到的轮次不早于读之前已完成的轮次"和 Scan 有序/完整；4 个线程并发写 + 删除后全量校验。数据量用 `KV_STRESS_KEYS` 调。
- **ThreadSanitizer**：`scripts/run_tsan.sh`，35 个测试全部通过，0 个数据竞争报告。

已知问题：一次大压缩期间不能刷盘，写入速度很快时会因冻结表未刷完而停顿（单后台线程的代价）。

## Phase 7：网络服务层 ✅

- `include/kv/protocol.h`：文本协议 `SET/GET/DEL/SCAN/PING`（格式见头文件）。
- `include/kv/server.h`：epoll 单 Reactor，非阻塞 socket，按行分帧（处理粘包/拆包），输出背压，eventfd 优雅退出。
- `include/kv/client.h` + `tools/kv_cli.cpp`：阻塞式客户端（交互模式 / 单条命令 / 管道输入）。
- `tools/kv_server.cpp`：`kv_server --port 6380 --dir ./kvdata`，Ctrl+C 优雅退出。
- 测试：`tests/server_test.cpp`——协议单元测试；真实 TCP：读写、管道化多条命令、半条命令分多次发送、8 个客户端并发（TSan 下也通过）。

手动体验：

```bash
./build/kv_server --port 6380 --dir /tmp/kvdata &
./build/kv_cli --port 6380                # 交互模式
./build/kv_cli --port 6380 SET name alice # 单条命令
printf 'SET a 1
GET a
SCAN a -
' | ./build/kv_cli --port 6380
```

端到端验证（已执行）：写入 → DEL → SCAN → Ctrl+C 退出 → 重启服务端 → SCAN 结果一致。

已知问题：命令在 I/O 线程同步执行，一次大 SCAN 或写停顿会让所有连接一起等待（单 Reactor 的固有限制）。

## Phase 8：性能基准 ✅

- `bench/db_bench.cpp`：顺序写、随机写、随机读（存在/不存在）、范围扫描；输出 ops/s、平均/p50/p99 延迟，以及写放大、各层文件数、每次读平均读了几个数据块。
- `scripts/run_bench.sh [构建目录] [条数]`：一键跑下面的对比矩阵。
- 新增统计接口 `DB::GetStats()`（用户写入字节、刷盘/压缩写出字节、压缩次数、数据块读取次数）。

**测试环境**：i7-12700H（20 线程），WSL2 Ubuntu 24.04，ext4，Release `-O2`。50 万条，键 16B、值 100B（约 58MB）。
数据都在 OS 页缓存中（"热读"），每组只跑一次，波动约 ±10%。

### 1. 基线（布隆过滤器开，MemTable 4MB）

| benchmark | ops/s | avg(us) | p50(us) | p99(us) | 备注 |
|---|---:|---:|---:|---:|---|
| fillseq | 369,652 | 2.67 | 1.17 | 4.31 | 写放大 2.13；L0/L1/L2 = 1/5/25 |
| fillrandom | 226,253 | 4.39 | 1.84 | 7.59 | 写放大 4.60；压缩 21 次 |
| readrandom | 60,921 | 16.39 | 15.39 | 34.32 | 每次读 0.99 个数据块 |
| readmissing | 941,405 | 1.04 | 0.72 | 15.29 | 每次读 0.013 个数据块 |
| scan(100 条) | 760 | 1,316 | 1,159 | 3,521 | 合计 7.6 万条/s |

### 2. 布隆过滤器开 / 关（MemTable 4MB）

| benchmark | 开：ops/s | 关：ops/s | 开：每次读块数 | 关：每次读块数 |
|---|---:|---:|---:|---:|
| readrandom | 60,921 | 46,661 | 0.993 | 1.388 |
| readmissing | **941,405** | 43,213 | **0.013** | 1.539 |

### 3. MemTable 阈值 1MB / 4MB / 16MB（布隆过滤器开）

| MemTable | fillrandom ops/s | 写 avg / p50 (us) | 写放大 | 压缩次数 | readrandom ops/s | scan ops/s |
|---|---:|---:|---:|---:|---:|---:|
| 1MB | 74,753 | 13.35 / 1.13 | 8.61 | 44 | 74,118 | 3,189 |
| 4MB | 226,253 | 4.39 / 1.84 | 4.60 | 21 | 60,921 | 760 |
| 16MB | 274,014 | 3.62 / 2.55 | 1.74 | 1 | 42,979 | 91 |

### 差异原因（面试可讲）

1. **布隆过滤器对"不存在的键"效果最大（22 倍）**：查不存在的键，没有过滤器时要在每个键范围覆盖它的文件（L0 + 每层一个）里都读一个块（1.54 块/次）；有过滤器时 99% 以上的文件直接跳过（0.013 块/次 ≈ 1% 误判率 × 文件数）。对存在的键也有 30% 提升：键所在层之上的文件被过滤器跳过，块数从 1.39 降到 ~1。
2. **点查的成本几乎全在"读一整块并解析"**：readrandom 每次约 15us，而 readmissing 在过滤器挡住时只有 0.7us。每次命中都要 pread 4KB 并把整块解析成 ~35 个 `std::string`。改进方向：块缓存（Block Cache）、块内重启点 + 二分（不用解析整块）——见 LEARNING_GUIDE 练习。
3. **顺序写的写放大明显更低（2.1 vs 4.6）**：顺序键的 SSTable 之间互不重叠，L1→L2 时下层没有重叠文件可以直接 trivial move；随机键每次压缩都要和下层重叠文件一起重写。fillseq 写放大仍有 2 而不是 1，是因为 L0→L1 一次取全部 L0 文件、总要重写一遍（LevelDB 只取一个 L0 文件 + 与之重叠的，顺序写时可以 trivial move——这是本项目简化的代价）。
4. **MemTable 越小，写放大越大、写停顿越严重**：1MB 时写放大 8.6，压缩 44 次；平均延迟 13us 但 p50 只有 1.1us——说明大部分写很快，少数写在等后台（冻结表没刷完或 L0 达到 12 个文件时写停顿），平均值被拉高。16MB 时 L0 文件大、数量少，数据在内存里多停留，写放大降到 1.7。
5. **MemTable 越大，点查和 Scan 越慢**：
   - Scan 慢得最明显（16MB 时每次 11ms）：`MemTable::NewIterator` 会把整张内存表拷贝成快照（为了并发安全、迭代时不持锁，见 memtable.cpp 的 `[性能]` 注释），代价与内存表大小成正比，而与扫描条数无关。这是"实现简单 + 无锁迭代"换来的。改进：只拷贝 [start, start+limit) 的部分、或改用支持并发读的跳表直接迭代。
   - readrandom 在 16MB 时也更慢（43k vs 61k ops/s）。可能原因：先查的内存表更大，跳表节点分散在堆上，指针跳转缓存不命中更多；L0 留下的 2 个 16MB 文件互相重叠、都要检查。未进一步用 perf 证实。
6. **读写不对称是 LSM-tree 的本质**：写只是 WAL 追加 + 内存表插入（1~2us），读却可能要查多层；所有的过滤器、压缩策略都是在用写放大/空间换读放大。

已知问题：基准是单线程的；没有测多线程读的扩展性，也没有测 `sync_wal=true`（每次写 fdatasync）的情况。

## Phase 9：学习文档 ✅

- `docs/LEARNING_GUIDE.md`：21 步代码阅读顺序（每步读完应懂什么）；LSM-tree 三种放大与权衡、与 B+ 树对比；15 道面试题 + 1 道附加题（C++ / 操作系统 / 存储 / 网络，标注答案所在文件和行号）；9 个动手练习；3 分钟项目讲法。
- `README.md`：项目概览与快速开始。
- `docs/BLOCKERS.md`：无未解决问题；记录了两个环境问题的解决办法。

## 总体状态

全部 9 个阶段完成。41 个测试在 Release 和 ThreadSanitizer 构建下均通过；测试数据都写在系统临时目录并自动清理。

已知限制（都是有意的简化，见 DECISIONS.md，可作为练习）：没有块缓存和块内前缀压缩；MemTable 迭代是整表快照拷贝（大 MemTable 下 Scan 慢）；单后台线程；单 Reactor；不支持快照读；仅支持 Linux。
