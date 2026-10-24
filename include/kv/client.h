// client.h —— 协议客户端（阻塞式），供命令行工具和测试使用
//
// 面试可能问：
//   - 客户端怎么知道一条响应读完了？
//     答：和服务端切命令一样靠协议边界：普通命令的响应是一行；SCAN 的响应读到 "END"（或 "ERR"）行为止。
//   - 为什么要自己做缓冲读，不能每次 read 一行？
//     答：read 拿到的是"当前到达的任意多字节"，可能是半行，也可能是好几行。
//     必须把字节攒在缓冲里，自己找 '\n'。
#pragma once

#include <cstdint>
#include <string>

namespace kv {

class Client {
 public:
  // 连接 host:port（host 为 IPv4 地址），失败抛 IOError
  Client(const std::string& host, uint16_t port);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // 发送一行命令（不含换行），返回完整响应（含换行）。连接断开抛 IOError
  std::string Command(const std::string& line);

  // 底层操作：原样发送字节、读一行（不含 '\n'）。测试"粘包/拆包"时用
  void SendRaw(const std::string& bytes);
  std::string ReadLine();

 private:
  int fd_ = -1;
  std::string buffer_;  // 已收到但还没被 ReadLine 取走的字节
};

}  // namespace kv
