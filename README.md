# lsm-kv

一个用 C++20 从零实现的 LSM-tree 键值存储引擎，结构参考 LevelDB，面向学习与面试讲解。只依赖标准库和 Linux 系统调用。

**功能**

- 跳表 MemTable（墓碑删除）
- 预写日志 WAL（CRC32 校验，写到一半崩溃可正确截断恢复）
- SSTable：数据块 + 布隆过滤器 + 索引 + footer，每块带校验和
- MANIFEST 原子更新、孤儿文件清理，刷盘/压缩任意时刻崩溃都可恢复
- 4 层 Leveled Compaction（丢弃旧版本，墓碑在无更深旧版本时回收，trivial move）
- 单写者 + 多读者并发，后台线程刷盘与压缩，写停顿背压
- epoll 单 Reactor TCP 服务（`SET / GET / DEL / SCAN`）+ 命令行客户端
- `db_bench` 基准：吞吐、p50/p99、写放大、每次读的数据块数

**测试**：41 个 GoogleTest 用例，包括"在 WAL 的每一个字节位置截断"、与 `std::map` 的随机对拍（含重启和多层压缩）、多线程压力测试，ThreadSanitizer 下 0 报告。

## 快速开始

需要 Linux（或 WSL2）、g++ ≥ 11、CMake ≥ 3.20。

```bash
scripts/build_and_test.sh          # 编译并运行全部测试
scripts/run_tsan.sh                # ThreadSanitizer 构建并运行测试
scripts/run_bench.sh build 500000  # 基准对比矩阵

./build/kv_server --port 6380 --dir /tmp/kvdata &
./build/kv_cli --port 6380 SET hello world
./build/kv_cli --port 6380 GET hello
```

## 文档

- [docs/LEARNING_GUIDE.md](docs/LEARNING_GUIDE.md)：代码阅读顺序、LSM-tree 核心概念、15 道面试题（标注答案在代码哪里）、动手练习
- [docs/PROGRESS.md](docs/PROGRESS.md)：各阶段完成内容、运行方式、基准结果与分析
- [docs/DECISIONS.md](docs/DECISIONS.md)：每个设计决策的原因与备选方案
