// kv_cli —— 命令行客户端
//   交互模式：kv_cli [--host 127.0.0.1] [--port 6380]，然后逐行输入命令
//   单条命令：kv_cli --port 6380 GET mykey
//   也可以从管道读：printf 'SET a 1\nGET a\n' | kv_cli
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "kv/client.h"

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 6380;
  std::string command;  // 非空表示单条命令模式
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else {
      command += (command.empty() ? "" : " ") + arg;  // 剩下的参数拼成一条命令
    }
  }

  try {
    kv::Client client(host, port);
    if (!command.empty()) {
      std::cout << client.Command(command);
      return 0;
    }
    const bool interactive = ::isatty(STDIN_FILENO);
    std::string line;
    while (true) {
      if (interactive) std::cout << host << ":" << port << "> " << std::flush;
      if (!std::getline(std::cin, line)) break;
      if (line.empty()) continue;
      if (line == "quit" || line == "exit") break;
      std::cout << client.Command(line) << std::flush;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
