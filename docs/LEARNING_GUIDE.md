# 学习指南

这份文档帮你**按顺序读懂这个项目**，并准备面试。每个头文件顶部也有"对应 LevelDB 哪个组件 / 面试可能问什么"，读代码时一起看。

---

## 一、推荐阅读顺序

按"数据从写入到落盘再到被读出"的顺序读。每一步列出读完应该能回答的问题。

| 顺序 | 文件 | 读完应该懂什么 |
|---|---|---|
| 1 | `include/kv/types.h` | 为什么删除要写"墓碑"；为什么查找结果是三态（找到 / 已删除 / 不知道）。 |
| 2 | `include/kv/skiplist.h` | 跳表的插入和查找；`FindGreaterOrEqual` 里 `prev[]` 数组的作用；为什么概率取 1/4。 |
| 3 | `include/kv/memtable.h` + `src/memtable.cpp` | 内存表如何估算内存占用；为什么迭代器要拷贝快照；读写锁怎么用。 |
| 4 | `include/kv/iterator.h` | 统一的迭代器接口——后面 SSTable、归并都基于它。 |
| 5 | `include/kv/coding.h`、`src/crc32.cpp` | 小端编码；`char` 有符号带来的坑；CRC 查表法。 |
| 6 | `include/kv/file.h` + `src/file.cpp` | `write` 的短写和 EINTR；`fsync` vs `fdatasync`；为什么要 fsync 目录；`pread` 为什么线程安全；rename 原子替换。 |
| 7 | `include/kv/wal.h` + `src/wal.cpp` | WAL 记录格式；重放如何识别"写到一半"的记录；为什么坏记录之后的都要丢。 |
| 8 | `include/kv/sstable_format.h` + `src/sstable_format.cpp` | SSTable 文件布局；footer 为什么定长且在末尾；每块 crc。 |
| 9 | `include/kv/sstable.h`、`src/sstable_builder.cpp`、`src/sstable.cpp` | 写表：攒块 → 刷块 → 写索引；读表：索引二分 → 读一个块；两级迭代器；`enable_shared_from_this` 的用途。 |
| 10 | `include/kv/bloom.h` + `src/bloom.cpp` | 布隆过滤器原理、误判率公式、双重哈希；为什么墓碑的键也要放进过滤器。 |
| 11 | `include/kv/version.h` + `src/version.cpp` | 不可变 Version（copy-on-write）；MANIFEST 记录了什么；L0 和 L1+ 的查找有何不同。 |
| 12 | `include/kv/merging_iterator.h` + `src/merging_iterator.cpp` | k 路归并；同一个键多版本时如何只保留最新。 |
| 13 | `include/kv/compaction.h` + `src/compaction.cpp` | 分层压缩的触发分数；怎么挑文件；何时可以丢弃墓碑；trivial move。 |
| 14 | `include/kv/db.h` | 并发模型总览（单写者、SuperVersion、后台线程、加锁顺序）。 |
| 15 | `src/db.cpp` | 打开与恢复流程；写路径（先 WAL 后内存表）；写停顿；读路径。 |
| 16 | `src/db_background.cpp` | 后台循环；刷盘/压缩怎样"持锁取状态 → 放锁做 I/O → 持锁装新版本"；崩溃安全的操作顺序。 |
| 17 | `include/kv/protocol.h`、`src/protocol.cpp` | 文本协议与解析；为什么协议层与网络层分开。 |
| 18 | `include/kv/server.h` + `src/server.cpp` | epoll 单 Reactor；非阻塞 I/O；按行分帧（粘包/拆包）；EPOLLOUT 何时关注；背压；eventfd 唤醒。 |
| 19 | `src/client.cpp`、`tools/` | 阻塞式客户端；TCP_NODELAY。 |
| 20 | `bench/db_bench.cpp` + `docs/PROGRESS.md` 的 Phase 8 | 怎么测吞吐和分位延迟；每个数字背后的原因。 |
| 21 | `tests/` | 特别看 `wal_test.cpp`（每个字节位置截断）、`db_fuzz_test.cpp`（与 std::map 对拍）、`concurrency_test.cpp`（怎样设计并发下可验证的不变量）。 |

建议边读边跑：`scripts/build_and_test.sh`，然后挑一个测试加断点单步调试（例如 `DBTest.FlushToSSTableAndReadBackAfterReopen`），看一次写入怎样一路走到 SSTable。

---

## 二、LSM-tree 核心概念

### 1. 结构一句话

写入先追加到 WAL、再插入内存里的有序表（MemTable）；MemTable 满了就整个冻结、顺序写成一个不可变的有序文件（SSTable）；后台不断把小文件合并成大文件（Compaction）。读的时候从新到旧依次查：MemTable → 冻结的 MemTable → L0 → L1 → …

### 2. 三种放大

| 放大 | 定义 | 本项目中的来源 | 怎么降低 |
|---|---|---|---|
| **写放大**（Write Amplification） | 实际写入磁盘的字节 / 用户写入的字节 | WAL 写一次、刷盘写一次、每次压缩再写一次。Leveled 下每层大约 10 倍，理论上限 ≈ 1 + 层数 × 10 | 更大的 MemTable、Tiered 压缩、更少层数、键值分离（WiscKey） |
| **读放大**（Read Amplification） | 一次读实际访问的数据量 / 需要的数据 | L0 每个文件都可能要查；L1+ 每层一个文件 | 布隆过滤器、少留 L0 文件、块缓存 |
| **空间放大**（Space Amplification） | 磁盘占用 / 有效数据大小 | 被覆盖的旧版本、墓碑在压缩前一直占空间 | 更积极的压缩（代价是写放大） |

**三者不可兼得**（RUM 猜想：Read、Update、Memory 三者最多优化两个）。
- Leveled：读放大、空间放大小，写放大大。适合读多写少。
- Tiered：写放大小，读放大、空间放大大。适合写多读少。

本项目实测（PROGRESS.md Phase 8）：随机写 50 万条，MemTable 4MB 时写放大 4.6；MemTable 16MB 时 1.7；1MB 时 8.6。

### 3. 和 B+ 树对比

| | LSM-tree | B+ 树（InnoDB 等） |
|---|---|---|
| 写 | 顺序追加，快；但有后台压缩的写放大 | 原地更新，随机写页；页分裂；需要 redo log + 双写/全页写防撕裂 |
| 读 | 可能查多层，依赖布隆过滤器 | 一次从根到叶，路径稳定（3~4 层） |
| 范围扫描 | 多路归并，有额外开销 | 叶子链表顺序读，很快 |
| 空间 | 旧版本待压缩回收；但文件紧凑、易压缩 | 页内有空洞（填充率 ~70%） |
| 延迟稳定性 | 压缩会造成毛刺和写停顿 | 相对稳定 |
| 典型系统 | LevelDB、RocksDB、Cassandra、HBase、TiKV | MySQL InnoDB、PostgreSQL、SQLite |

一句话：**LSM 用"后台合并"把随机写变成顺序写，换取高写吞吐；B+ 树用"原地更新"换取稳定的读。**

---

## 三、面试题（15 道，答案位置已标注）

### C++

**1. 为什么 `Table` 继承 `std::enable_shared_from_this`？如果在成员函数里直接 `std::shared_ptr<Table>(this)` 会怎样？**
答案位置：`include/kv/sstable.h:61`、`src/sstable.cpp:103`。
要点：迭代器要共享 Table 的所有权，防止迭代期间表被压缩删除后析构。直接用 `this` 新建 shared_ptr 会产生第二个独立的控制块，引用计数各算各的，导致重复析构。

**2. 跳表为什么禁止拷贝？`InsertOrAssign` 为什么返回 `std::optional<Value>`？**
答案位置：`include/kv/skiplist.h:58`、`src/memtable.cpp:17`。
要点：节点是裸指针互指，默认拷贝是浅拷贝，析构两次就 double free（三/五法则）。返回被覆盖的旧值，MemTable 才能精确修正内存统计，同时避免"先 Find 再 Insert"走两遍。

**3. `DecodeFixed32` 里为什么要先转成 `unsigned char`？**
答案位置：`include/kv/coding.h:28`。
要点：`char` 在 x86 上通常是有符号的，0x80 以上的字节会被符号扩展成 0xFFFFFF80，或运算后高位全被污染。

**4. 后台线程里抛出异常时，`unique_lock` 可能处于什么状态？代码怎么处理的？**
答案位置：`src/db_background.cpp:17`（`BackgroundLoop` 的 catch 分支）。
要点：刷盘/压缩会临时 `unlock()` 做 I/O，此时抛异常，锁处于未持有状态；catch 里先 `if (!lock.owns_lock()) lock.lock();` 再写共享状态，否则就是数据竞争，随后的 `wait` 也是未定义行为。

### 操作系统

**5. `write()` 返回成功，数据就安全了吗？`fsync`、`fdatasync`、fsync 目录分别解决什么问题？**
答案位置：`include/kv/file.h` 顶部、`src/file.cpp:59`、`src/file.cpp:117`、`src/file.cpp:125`。
要点：write 只进页缓存（进程崩溃不丢，掉电会丢）；fsync 刷数据和全部元数据，fdatasync 只刷数据和必要元数据；新建文件/rename 改的是目录项，要 fsync 目录才持久。

**6. 压缩删除了一个 SSTable 文件，但另一个线程还在读它，会出错吗？**
答案位置：`include/kv/db.h` 顶部"并发模型"、`src/db_background.cpp:156`。
要点：Linux 上 unlink 只删目录项，已打开的 fd 仍可读，inode 在最后一个 fd 关闭时才释放。读者持有旧 Version 的 shared_ptr → Table → fd。Windows 上不行（打开的文件不能删），这也是为什么本项目只支持 Linux。

**7. 加锁顺序是什么？为什么读者只在锁里拷贝三个指针？**
答案位置：`include/kv/db.h` 顶部、`src/db.cpp:176`、`src/db.cpp:122`。
要点：固定顺序 `write_mu_ → mu_ → MemTable 内部锁`，避免环形等待死锁。读者锁内只拷贝 shared_ptr（SuperVersion），锁外读不可变数据，锁持有时间极短，长 Scan 也不会挡住写者切换 MemTable。

**8. 什么是写停顿（write stall）？本项目何时会发生？**
答案位置：`src/db.cpp:142`、`include/kv/options.h`（`l0_stop_writes_trigger`）。
要点：写入速度长期超过刷盘/压缩速度时必须让写者等，否则内存或 L0 无限增长。本项目在"已有一张冻结表未刷完"或"L0 ≥ 12 个文件"时等待。基准中 MemTable 1MB 的写平均延迟远大于 p50，就是写停顿造成的长尾。

### 存储

**9. 写到一半崩溃，WAL 末尾是什么样？怎么恢复？为什么坏记录之后的数据都丢掉而不是跳过？**
答案位置：`include/kv/wal.h` 顶部、`src/wal.cpp:52`、`tests/wal_test.cpp`（TruncationAtEveryOffset）。
要点：可能是半条记录或校验和不对；遇到第一条坏记录就停。只恢复前缀，保证恢复出的是某个真实存在过的状态；且坏记录后的长度字段不可信，无法定位下一条。

**10. 刷盘过程中在任意一步崩溃，数据会丢吗？逐步说明。**
答案位置：`include/kv/db.h` 顶部、`src/db_background.cpp:52`、`src/db.cpp:89`（编号修正）。
要点：① SSTable 写完 fsync ② 原子写 MANIFEST（推进 log_number）③ 删旧 WAL。①②之间崩溃：孤儿 SSTable 启动时删除、WAL 重放；②③之间：旧 WAL 编号 < log_number，启动时删除。另外 MANIFEST 里的 next_file_number 可能落后，启动时取目录中最大编号 + 1。

**11. 墓碑什么时候可以真正删除？过早删除会怎样？**
答案位置：`src/compaction.cpp:39`（`IsBaseLevelForKey`）、`tests/compaction_test.cpp`。
要点：只有确定更深的层没有该键的旧版本时才能丢。过早丢弃会让更深层的旧值"复活"。

**12. 布隆过滤器误判率怎么算？为什么对查不存在的键提升 22 倍，对存在的键只提升 30%？**
答案位置：`include/kv/bloom.h` 顶部、`src/bloom.cpp:23`、PROGRESS.md Phase 8 表 2。
要点：p ≈ (1 − e^(−kn/m))^k，10 bit/key、k=7 时约 0.8%。不存在的键原本要在每个范围覆盖它的文件里读一块，过滤器几乎全挡掉；存在的键最终总要读包含它的那一块，过滤器只省掉上层不相关文件的读。

**13. 为什么 L0 允许文件重叠而 L1+ 不允许？L0→L1 压缩为什么一次取全部 L0 文件？**
答案位置：`include/kv/version.h`、`src/version.cpp:20`、`src/compaction.cpp:95`。
要点：L0 直接来自 MemTable，每个文件覆盖任意范围，所以查找要逐个查；L1+ 不重叠才能每层二分只查一个文件。L0 只取一部分会让更旧的数据留在更浅的层，破坏"越深越旧"；全取最简单（代价：顺序写时也要重写，见基准分析第 3 点）。

### 网络

**14. TCP 粘包/拆包是什么？服务端怎么处理？**
答案位置：`src/server.cpp:153`（`ProcessCommands`）、`tests/server_test.cpp`（PipeliningAndPartialLines）。
要点：TCP 是字节流没有消息边界；一次 read 可能拿到半条或多条命令。把字节攒进输入缓冲，按 '\n' 切出完整命令，剩下的半条留到下次。

**15. epoll 水平触发和边缘触发的区别？为什么不一直关注 EPOLLOUT？客户端只发不收会怎样？**
答案位置：`include/kv/server.h` 顶部、`src/server.cpp:171`、`src/server.cpp:192`。
要点：LT 有数据就一直通知，ET 只在变化时通知一次、必须读到 EAGAIN。发送缓冲通常可写，一直关注 EPOLLOUT 会让 epoll_wait 空转；只在输出缓冲非空时关注。只发不收会让输出缓冲无限增长——本项目在超过 4MB 时暂停读该连接（背压）。

**附加题：为什么 Stop() 能在信号处理函数里调用？**
答案位置：`src/server.cpp:82`、`tools/kv_server.cpp`。
要点：信号处理函数里只能调用异步信号安全的函数（不能 malloc、不能加锁）；Stop 只做原子写和 `write(eventfd)`，write 是异步信号安全的。

---

## 四、动手练习（由易到难）

每个练习完成后都应该让现有测试（尤其是对拍测试）继续通过，并在 `docs/DECISIONS.md` 记下你的取舍。

1. **varint 编码**：把 WAL 和数据块里的 4 字节定长长度改成 varint（`coding.h`），对比 SSTable 体积变化。注意改了格式，旧数据目录不能再打开——想一想如何在 footer 里加版本号做兼容。
2. **块内前缀压缩 + 重启点**：数据块里每个键只存与前一个键不同的后缀，每 16 条放一个完整键（重启点），块尾存重启点偏移数组。`Table::Get` 改为先在重启点上二分，再线性扫描，不再解析整块。用 `db_bench --benchmarks=readrandom` 对比（预期点查明显变快，文件变小）。
3. **LRU 块缓存**：给 `Table::ReadDataBlock` 加一个所有表共享、按字节计容量的 LRU 缓存（`std::list` + `std::unordered_map`，加锁）。思考：缓存 key 用什么（文件编号 + 块偏移）？并发下怎么保证安全？
4. **给 SSTable 加块压缩**：实现一个简单的压缩算法（如 RLE，或手写 LZ77 简化版），数据块写入前压缩，读出后解压；块头加 1 字节表示是否压缩。测一测随机值和重复值两种数据的效果。
5. **Scan 的 MemTable 只拷贝需要的部分**：`MemTable::NewIterator` 目前拷贝整张表（基准里 16MB 时一次 Scan 要 11ms）。改成 `NewIterator(start, limit)` 只拷贝 `[start, …)` 的前 limit 条，或者干脆在锁内迭代。重新跑 `scan` 基准对比。
6. **组提交（Group Commit）**：`sync_wal=true` 时每次写都 fdatasync，非常慢。让并发写者排队，队头写者把队列里所有记录合并成一次 write + 一次 fdatasync（参考 LevelDB `DBImpl::Write` 的 writers_ 队列）。用多线程写基准对比。
7. **L0→L1 只挑一个文件**：参考 LevelDB，挑一个 L0 文件并扩展到所有与之重叠的 L0 文件，让顺序写也能 trivial move。验证 fillseq 写放大降到接近 1。
8. **多 Reactor**：主线程只 accept，按轮询把连接分给 N 个 I/O 线程，每个线程自己一个 epoll（one loop per thread）。注意：DB 本身已经线程安全，但连接对象只能被所属线程访问。用多个客户端并发压测对比。
9. **快照读（较难）**：给每条记录加全局递增序列号，MemTable 存 `(key, seq)` 多版本，`GetSnapshot()` 返回一个序列号，读时忽略更新的版本；压缩时不能丢弃仍被快照引用的旧版本。这会触及 `types.h`、MemTable、SSTable 格式、归并迭代器和压缩——是理解 MVCC 的最好练习。

---

## 五、简历/面试怎么讲这个项目

一个 3 分钟的讲法：

1. **做了什么**：从零用 C++20 实现的 LSM-tree KV 存储引擎，包括跳表 MemTable、带 CRC 的 WAL 与崩溃恢复、SSTable（数据块 + 布隆过滤器 + 索引 + footer）、分层压缩、单写多读并发与后台线程、epoll 文本协议服务。核心代码约 3000 行（含大量中文注释），41 个单元测试，含与 `std::map` 的随机对拍和 ThreadSanitizer 检查。
2. **一个正确性难点**：并发后发现"后台压缩时可能把冻结表还没刷盘的 WAL 当作过期删掉"——同步模型下不存在的问题。用 `MinLiveLogNumber()` 解决（`include/kv/db.h`）。说明你会思考崩溃一致性。
3. **一个性能分析**：布隆过滤器让查不存在的键快 22 倍、每次读的数据块从 1.54 降到 0.013；MemTable 从 4MB 调到 16MB，写放大从 4.6 降到 1.7，但 Scan 变慢 8 倍——因为 MemTable 迭代用的是快照拷贝。说明你能用数据解释取舍。
4. **知道不足**：没有块缓存、没有前缀压缩、单后台线程、单 Reactor——并能说出每个怎么改（上面的练习）。
