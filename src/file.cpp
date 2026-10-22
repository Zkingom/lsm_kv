#include "kv/file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace kv {
namespace {

[[noreturn]] void ThrowErrno(const std::string& what, const std::string& path) {
  throw IOError(what + " " + path + ": " + std::strerror(errno));
}

uint64_t FileSizeOfFd(int fd, const std::string& path) {
  struct stat st;
  if (::fstat(fd, &st) != 0) ThrowErrno("fstat", path);
  return static_cast<uint64_t>(st.st_size);
}

}  // namespace

// ---------------- WritableFile ----------------

WritableFile::WritableFile(const std::string& path, bool truncate) : path_(path) {
  int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
  if (truncate) flags |= O_TRUNC;
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) ThrowErrno("open", path);
  size_ = FileSizeOfFd(fd_, path);
}

WritableFile::~WritableFile() {
  if (fd_ >= 0) ::close(fd_);
}

void WritableFile::Append(std::string_view data) {
  // [性能] 每次 Append 直接一次 write() 系统调用，没有用户态缓冲。
  // 好处：调用返回后数据一定已在内核里，进程崩溃不会丢；代价：小写入时系统调用开销大。
  // LevelDB 有 64KB 用户态缓冲，靠显式 Flush 交给内核。
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;  // 被信号打断，重试
      ThrowErrno("write", path_);
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  size_ += data.size();
}

void WritableFile::Sync() {
  // fdatasync 只刷数据和必要的元数据（如文件长度），比 fsync 少刷一次 inode 时间戳
  if (::fdatasync(fd_) != 0) ThrowErrno("fdatasync", path_);
}

// ---------------- RandomAccessFile ----------------

RandomAccessFile::RandomAccessFile(const std::string& path) : path_(path) {
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0) ThrowErrno("open", path);
  size_ = FileSizeOfFd(fd_, path);
}

RandomAccessFile::~RandomAccessFile() {
  if (fd_ >= 0) ::close(fd_);
}

std::string RandomAccessFile::Read(uint64_t offset, size_t n) const {
  std::string buf(n, '\0');
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd_, buf.data() + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("pread", path_);
    }
    if (r == 0) throw IOError("unexpected end of file: " + path_);
    done += static_cast<size_t>(r);
  }
  return buf;
}

// ---------------- FileLock ----------------

FileLock::FileLock(const std::string& path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) ThrowErrno("open", path);
  // LOCK_NB：拿不到锁立刻失败，而不是一直等
  if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    int saved = errno;
    ::close(fd_);
    errno = saved;
    ThrowErrno("lock (is another process using this directory?)", path);
  }
}

FileLock::~FileLock() {
  if (fd_ >= 0) ::close(fd_);  // close 会自动释放 flock
}

// ---------------- 工具函数 ----------------

std::string ReadFileToString(const std::string& path) {
  RandomAccessFile file(path);
  if (file.Size() == 0) return std::string();
  return file.Read(0, file.Size());
}

void SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) ThrowErrno("open dir", dir);
  int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) ThrowErrno("fsync dir", dir);
}

void WriteFileAtomically(const std::string& path, std::string_view content) {
  // [持久化] rename 在同一文件系统内是原子的：读者要么看到完整的旧文件，要么看到完整的新文件，
  // 不会看到写了一半的内容。前提是 rename 之前新文件内容已经 fsync。
  std::string tmp = path + ".tmp";
  {
    WritableFile file(tmp, /*truncate=*/true);
    file.Append(content);
    file.Sync();
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) ThrowErrno("rename", tmp);
  SyncDir(std::filesystem::path(path).parent_path().string());
}

}  // namespace kv
