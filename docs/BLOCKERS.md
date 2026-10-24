# 阻塞问题

目前没有未解决的阻塞问题。

开发过程中遇到并已解决的环境问题（记录在此，方便在新机器上复现）：

- **CMake FetchContent 下载 GoogleTest 偶发 SSL 失败**（WSL 经代理访问 GitHub）。解决：手动下载 googletest v1.14.0 源码，配置时加 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<源码目录>`。网络正常的机器不需要。
- **ThreadSanitizer 报 `FATAL: ThreadSanitizer: unexpected memory mapping`**（新内核 ASLR 随机位数过大）。解决：用 `setarch $(uname -m) -R` 在关闭 ASLR 的进程里运行测试，`scripts/run_tsan.sh` 已内置。
