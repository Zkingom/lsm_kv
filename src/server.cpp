#include "kv/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "kv/protocol.h"

namespace kv {
namespace {

constexpr size_t kMaxLineLength = 1 << 20;      // 单条命令最长 1MB，超过视为恶意/错误客户端
constexpr size_t kMaxOutputBuffer = 4 << 20;    // 输出缓冲超过 4MB 暂停读（背压）
constexpr int kMaxEvents = 64;

[[noreturn]] void ThrowErrno(const std::string& what) {
  throw IOError(what + ": " + std::strerror(errno));
}

int CreateListenSocket(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) ThrowErrno("socket");
  // SO_REUSEADDR：服务重启时，旧连接还在 TIME_WAIT 也能立刻重新绑定同一端口
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);  // 网络字节序是大端，htons 负责转换
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    ThrowErrno("bind");
  }
  if (::listen(fd, SOMAXCONN) < 0) {
    ::close(fd);
    ThrowErrno("listen");
  }
  return fd;
}

uint16_t LocalPort(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) ThrowErrno("getsockname");
  return ntohs(addr.sin_port);
}

void EpollCtl(int epoll_fd, int op, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd, op, fd, &ev) < 0) ThrowErrno("epoll_ctl");
}

}  // namespace

Server::Server(DB* db, uint16_t port) : db_(db) {
  listen_fd_ = CreateListenSocket(port);
  port_ = LocalPort(listen_fd_);
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) ThrowErrno("epoll_create1");
  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) ThrowErrno("eventfd");
  EpollCtl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, EPOLLIN);
  EpollCtl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, EPOLLIN);
}

Server::~Server() {
  for (auto& [fd, conn] : conns_) ::close(fd);
  if (wake_fd_ >= 0) ::close(wake_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (listen_fd_ >= 0) ::close(listen_fd_);
}

void Server::Stop() {
  stopping_.store(true);
  uint64_t one = 1;
  // write 是异步信号安全的，所以 Stop 可以在 SIGINT 处理函数里调用
  [[maybe_unused]] ssize_t n = ::write(wake_fd_, &one, sizeof(one));
}

void Server::Run() {
  epoll_event events[kMaxEvents];
  while (!stopping_.load()) {
    int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, -1);
    if (n < 0) {
      if (errno == EINTR) continue;  // 被信号打断（如 SIGINT），回到循环顶检查 stopping_
      ThrowErrno("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      if (fd == wake_fd_) continue;  // 只是为了叫醒，循环条件会处理退出
      if (fd == listen_fd_) {
        AcceptAll();
        continue;
      }
      auto it = conns_.find(fd);
      if (it == conns_.end()) continue;  // 本轮早些时候已被关闭
      Connection& conn = it->second;
      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        CloseConnection(fd);
        continue;
      }
      if (events[i].events & EPOLLIN) HandleReadable(conn);
      // HandleReadable 可能已经关闭了连接，重新查一次
      if (conns_.count(fd) && (events[i].events & EPOLLOUT)) FlushOutput(conns_[fd]);
    }
  }
}

void Server::AcceptAll() {
  // 水平触发下一次 accept 一个也可以，但一次把排队的连接都接完能少几次 epoll_wait
  while (true) {
    int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      return;  // 如 EMFILE（文件描述符耗尽）：先不接了，等连接释放后下一轮再试
    }
    conns_[fd].fd = fd;
    EpollCtl(epoll_fd_, EPOLL_CTL_ADD, fd, EPOLLIN);
  }
}

void Server::HandleReadable(Connection& conn) {
  char buf[16 << 10];
  while (true) {
    ssize_t n = ::read(conn.fd, buf, sizeof(buf));
    if (n > 0) {
      conn.in.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {  // 对端关闭了写方向：处理完已收到的命令、发完响应再关
      conn.want_close = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 本轮数据读完了
    CloseConnection(conn.fd);
    return;
  }
  ProcessCommands(conn);
  FlushOutput(conn);
}

void Server::ProcessCommands(Connection& conn) {
  // 按 '\n' 切出所有完整的命令；最后不完整的半条留在 in 里等下次（处理"拆包"）
  size_t start = 0;
  while (true) {
    size_t newline = conn.in.find('\n', start);
    if (newline == std::string::npos) break;
    std::string_view line(conn.in.data() + start, newline - start);
    conn.out += ExecuteCommand(db_, line);
    start = newline + 1;
  }
  conn.in.erase(0, start);
  if (conn.in.size() > kMaxLineLength) {
    conn.out += "ERR line too long\n";
    conn.in.clear();
    conn.want_close = true;
  }
}

void Server::FlushOutput(Connection& conn) {
  while (!conn.out.empty()) {
    // MSG_NOSIGNAL：对端已关闭时返回 EPIPE，而不是用 SIGPIPE 信号杀死整个进程
    ssize_t n = ::send(conn.fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);
    if (n > 0) {
      // [性能] erase 头部是 O(剩余长度) 的拷贝；高性能实现会用环形缓冲或记录读偏移
      conn.out.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // 内核发送缓冲满了，等 EPOLLOUT
    CloseConnection(conn.fd);
    return;
  }
  if (conn.out.empty() && conn.want_close) {
    CloseConnection(conn.fd);
    return;
  }
  UpdateInterest(conn);
}

void Server::UpdateInterest(Connection& conn) {
  uint32_t events = 0;
  // 背压：输出积压太多（客户端只发不收）就先不读它的新命令；对端已关闭写方向也不必再读
  if (conn.out.size() < kMaxOutputBuffer && !conn.want_close) events |= EPOLLIN;
  if (!conn.out.empty()) events |= EPOLLOUT;  // 只有真有数据没发完时才关注可写
  EpollCtl(epoll_fd_, EPOLL_CTL_MOD, conn.fd, events);
}

void Server::CloseConnection(int fd) {
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  ::close(fd);
  conns_.erase(fd);
}

}  // namespace kv
