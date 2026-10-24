#include "kv/client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>

#include "kv/file.h"

namespace kv {
namespace {

[[noreturn]] void ThrowErrno(const std::string& what) {
  throw IOError(what + ": " + std::strerror(errno));
}

bool IsScanCommand(const std::string& line) {
  if (line.size() < 4) return false;
  for (size_t i = 0; i < 4; ++i) {
    if (std::toupper(static_cast<unsigned char>(line[i])) != "SCAN"[i]) return false;
  }
  return line.size() == 4 || line[4] == ' ';
}

}  // namespace

Client::Client(const std::string& host, uint16_t port) {
  fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) ThrowErrno("socket");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd_);
    throw IOError("invalid IPv4 address: " + host);
  }
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    int saved = errno;
    ::close(fd_);
    errno = saved;
    ThrowErrno("connect " + host + ":" + std::to_string(port));
  }
  // 关闭 Nagle 算法：小请求立即发出，不等凑够一个包（否则一问一答会多出几十毫秒延迟）
  int one = 1;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

Client::~Client() {
  if (fd_ >= 0) ::close(fd_);
}

void Client::SendRaw(const std::string& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("send");
    }
    sent += static_cast<size_t>(n);
  }
}

std::string Client::ReadLine() {
  while (true) {
    size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      return line;
    }
    char buf[16 << 10];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) ThrowErrno("recv");
    if (n == 0) throw IOError("connection closed by server");
    buffer_.append(buf, static_cast<size_t>(n));
  }
}

std::string Client::Command(const std::string& line) {
  SendRaw(line + "\n");
  std::string response;
  const bool multi_line = IsScanCommand(line);
  while (true) {
    std::string row = ReadLine();
    response += row + "\n";
    if (!multi_line || row == "END" || row.rfind("ERR", 0) == 0) return response;
  }
}

}  // namespace kv
