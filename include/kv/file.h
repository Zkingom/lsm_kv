// file.h —— 对 POSIX 文件系统调用的薄封装
//
// 对应组件：LevelDB include/leveldb/env.h + util/env_posix.cc。
// 面试可能问：
//   - write() 返回了就代表数据落盘了吗？
//     答：没有。write() 只是拷进内核页缓存。进程崩溃不会丢（内核还在），
//     但掉电/内核崩溃会丢。要真正落盘必须 fsync()/fdatasync()。
//   - 为什么新建文件后还要 fsync 目录？
//     答：文件内容 fsync 了，但"目录里有这个文件名"这条元数据在目录的数据块里，
//     掉电后可能文件名都找不到。rename 同理。
//   - pread 和 lseek+read 有什么区别？
//     答：pread 自带偏移量、不改文件位置指针，多个线程可以同时用同一个 fd 读不同位置，
//     不会互相干扰。Phase 6 的多读者依赖这一点。
//   - write() 为什么要循环？
//     答：write 可能只写了一部分（短写），也可能被信号打断返回 EINTR。
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kv {

// I/O 失败（磁盘满、权限、文件损坏等）统一抛这个异常。
// 选择异常而不是错误码的原因见 docs/DECISIONS.md。
class IOError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// 只追加写的文件（WAL、正在生成的 SSTable）
class WritableFile {
 public:
  // truncate=true 时清空已有内容，否则在末尾追加
  explicit WritableFile(const std::string& path, bool truncate = false);
  ~WritableFile();
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  void Append(std::string_view data);
  // [持久化] 把内核缓存刷到磁盘
  void Sync();
  uint64_t Size() const { return size_; }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
  uint64_t size_ = 0;
};

// 随机读文件（SSTable）。Read 是 const 且线程安全（基于 pread）。
class RandomAccessFile {
 public:
  explicit RandomAccessFile(const std::string& path);
  ~RandomAccessFile();
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // 读 [offset, offset+n)，读不满就抛 IOError
  std::string Read(uint64_t offset, size_t n) const;
  uint64_t Size() const { return size_; }

 private:
  std::string path_;
  int fd_ = -1;
  uint64_t size_ = 0;
};

// 进程级独占锁：防止两个进程同时打开同一个数据目录把文件写乱
class FileLock {
 public:
  explicit FileLock(const std::string& path);
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  int fd_ = -1;
};

std::string ReadFileToString(const std::string& path);
// [持久化] fsync 目录本身，让目录项（新文件名、rename、删除）落盘
void SyncDir(const std::string& dir);
// [持久化] 原子地用 content 替换 path：写临时文件 → fsync → rename → fsync 目录
void WriteFileAtomically(const std::string& path, std::string_view content);

}  // namespace kv
