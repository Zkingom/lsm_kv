// server.h —— 基于 epoll 的单 Reactor TCP 服务
//
// 对应组件：Redis 的 ae 事件循环 / muduo 的 EventLoop（单线程版）。
// 结构：一个线程跑 epoll_wait 循环，所有连接的读、解析、执行命令、写回都在这个线程里完成。
//   - 监听 socket 可读 → accept 新连接（非阻塞），注册 EPOLLIN
//   - 连接可读        → 读到 EAGAIN 为止，追加进输入缓冲，按 '\n' 切出完整命令逐条执行，
//                       响应追加进输出缓冲并尝试立即发送
//   - 连接可写        → 继续发送输出缓冲；发完就取消 EPOLLOUT 关注
//   - eventfd 可读    → 别的线程（或信号处理函数）调用了 Stop()，退出循环
//
// 面试可能问：
//   - 为什么 socket 要设成非阻塞？
//     答：单线程要同时服务很多连接，任何一次阻塞的 read/write 都会卡住所有人。
//     非阻塞 + epoll：只在"确实有数据/确实能写"时才去读写，读不到就返回 EAGAIN。
//   - 水平触发（LT）和边缘触发（ET）的区别？你用的哪个？
//     答：LT：只要缓冲区里还有数据，每次 epoll_wait 都会报告；ET：只在状态变化时报告一次，
//     必须一次读到 EAGAIN，否则剩下的数据再也不会被通知。这里用 LT（默认），更不容易写错；
//     同时也是读到 EAGAIN 为止，减少 epoll_wait 次数。
//   - 为什么不是一直关注 EPOLLOUT？
//     答：socket 发送缓冲区绝大多数时候都是可写的，一直关注会让 epoll_wait 不停返回，空转占满 CPU。
//     只有输出缓冲里有没发完的数据时才关注。
//   - 客户端只发不收，服务端输出缓冲会无限增长吗？
//     答：这里做了背压：输出缓冲超过 4MB 就暂停读这个连接，等它把响应收走。
//   - 单 Reactor 的瓶颈？
//     答：所有命令都在一个线程里执行，DB 操作慢（如大 SCAN、写停顿）会拖累所有连接。
//     改进：多 Reactor（主线程 accept，若干 I/O 线程各自一个 epoll）或者把命令执行交给线程池。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "kv/db.h"

namespace kv {

class Server {
 public:
  // 绑定 0.0.0.0:port 并开始监听；port 为 0 时由系统分配（测试用）。失败抛 IOError
  Server(DB* db, uint16_t port);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  uint16_t port() const { return port_; }

  // 运行事件循环，直到 Stop() 被调用
  void Run();
  // 线程安全，也可以在信号处理函数里调用（只做原子写和 write(2)）
  void Stop();

 private:
  struct Connection {
    int fd = -1;
    std::string in;   // 收到但还没处理完的字节（可能有半条命令）
    std::string out;  // 待发送的响应
    bool want_close = false;  // 发完剩余响应后关闭
  };

  void AcceptAll();
  void HandleReadable(Connection& conn);
  void ProcessCommands(Connection& conn);
  void FlushOutput(Connection& conn);
  void UpdateInterest(Connection& conn);
  void CloseConnection(int fd);

  DB* db_;
  uint16_t port_ = 0;
  int listen_fd_ = -1;
  int epoll_fd_ = -1;
  int wake_fd_ = -1;  // eventfd：Stop() 往里写一个数，把 epoll_wait 叫醒
  std::atomic<bool> stopping_{false};
  std::unordered_map<int, Connection> conns_;
};

}  // namespace kv
