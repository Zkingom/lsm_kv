// kv_server —— 启动 TCP 服务
//   用法：kv_server [--port 6380] [--dir ./kvdata] [--memtable_kb 4096]
//   Ctrl+C（SIGINT）或 SIGTERM 优雅退出。
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "kv/db.h"
#include "kv/server.h"

namespace {

kv::Server* g_server = nullptr;

// 信号处理函数里只能调用异步信号安全的函数；Server::Stop 只做原子写和 write(2)
void HandleSignal(int) {
  if (g_server != nullptr) g_server->Stop();
}

struct Args {
  uint16_t port = 6380;
  std::string dir = "./kvdata";
  size_t memtable_kb = 4096;
};

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string flag = argv[i];
    if (flag == "--port") {
      args.port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
    } else if (flag == "--dir") {
      args.dir = argv[i + 1];
    } else if (flag == "--memtable_kb") {
      args.memtable_kb = static_cast<size_t>(std::atoll(argv[i + 1]));
    } else {
      std::fprintf(stderr, "unknown flag %s\n", argv[i]);
      std::exit(2);
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  Args args = ParseArgs(argc, argv);
  try {
    kv::Options options;
    options.memtable_size = args.memtable_kb << 10;
    auto db = kv::DB::Open(options, args.dir);
    kv::Server server(db.get(), args.port);
    g_server = &server;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::printf("kv_server listening on port %u, data dir %s\n", server.port(), args.dir.c_str());
    std::fflush(stdout);
    server.Run();
    g_server = nullptr;
    std::printf("shutting down\n");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
