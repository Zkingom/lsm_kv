# 设计决策记录

每行一条：**决定** —— 原因 —— 备选方案。

## Phase 1：MemTable

- **MemTable 中同一个键只保留最新值（覆盖写），不存多版本序列号** —— 本项目不做快照读，多版本只会增加内存和实现复杂度；读路径"新层覆盖旧层"的语义仍然成立 —— 备选：LevelDB 式 `(user_key, seq, type)` 内部键，可支持快照/MVCC。
- **跳表读写都由锁保护，不实现 LevelDB 的"一写多读无锁"跳表** —— CLAUDE.md 要求不上无锁结构；锁的版本更容易读懂、也更容易用 TSan 验证 —— 备选：用 `std::atomic` 指针 + 内存序实现无锁读。
- **跳表节点用 `new` + `std::vector<Node*>` 存后继指针** —— 可读性优先 —— 备选：Arena 批量分配 + 柔性数组（LevelDB），分配次数更少。
- **跳表层高概率 1/4、最大 12 层、随机种子固定** —— 与 LevelDB 相同；固定种子让问题可复现 —— 备选：p=1/2（更多指针、更少比较）。
- **MemTable 迭代器是"拷贝快照"** —— 迭代期间无需持锁，Phase 6 并发下最简单安全 —— 备选：直接遍历跳表（需要无锁跳表或长时间持锁）。
- **内存占用按 键长+值长+固定 96 字节节点开销 估算；覆盖写只修正值长差** —— 足够判断"该刷盘了"，不需要精确 —— 备选：自定义分配器精确统计。
- **`Get` 返回 `std::optional<std::string>`** —— 空串是合法值，必须与"不存在"区分 —— 备选：LevelDB 的 `Status` + 输出参数。

## 构建与测试

- **GoogleTest 通过 FetchContent 拉取 v1.14.0** —— CLAUDE.md 要求；开发机 WSL 里 CMake 自带下载偶发 SSL 失败，所以本地用 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<已下载源码>` 指向手动下载的同版本源码，CMakeLists 本身不变 —— 备选：`find_package(GTest)` 用系统包（但 TSan 构建时系统包未插桩）、自写断言头文件。
- **构建目录放在仓库外（WSL 的 `~/build/lsm-kv`）** —— 仓库在 Windows 盘上，跨文件系统编译慢；`.gitignore` 也忽略了 `build/` —— 备选：仓库内 `build/`。
