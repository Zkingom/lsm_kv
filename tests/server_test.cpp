#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "kv/client.h"
#include "kv/db.h"
#include "kv/protocol.h"
#include "kv/server.h"
#include "test_util.h"

namespace kv {
namespace {

// ---------------- 协议层（不经过网络）----------------

class ProtocolTest : public ::testing::Test {
 protected:
  std::string Run(const std::string& line) { return ExecuteCommand(db_.get(), line); }

  test::TempDir tmp_;
  std::unique_ptr<DB> db_ = DB::Open(Options(), tmp_.path());
};

TEST_F(ProtocolTest, BasicCommands) {
  EXPECT_EQ(Run("PING"), "PONG\n");
  EXPECT_EQ(Run("GET a"), "NOT_FOUND\n");
  EXPECT_EQ(Run("SET a hello world"), "OK\n");  // 值可以含空格
  EXPECT_EQ(Run("GET a"), "VALUE hello world\n");
  EXPECT_EQ(Run("set b 2\r"), "OK\n");  // 命令不区分大小写，接受 \r\n
  EXPECT_EQ(Run("DEL a"), "OK\n");
  EXPECT_EQ(Run("GET a"), "NOT_FOUND\n");
}

TEST_F(ProtocolTest, Scan) {
  for (const char* k : {"a", "b", "c", "d"}) Run(std::string("SET ") + k + " v" + k);
  Run("DEL c");
  EXPECT_EQ(Run("SCAN a d"), "a va\nb vb\nEND\n");  // [a, d)，c 已删除
  EXPECT_EQ(Run("SCAN b -"), "b vb\nd vd\nEND\n");  // "-" 表示无上界
  EXPECT_EQ(Run("SCAN a - 1"), "a va\nEND\n");
  EXPECT_EQ(Run("SCAN x y"), "END\n");
}

TEST_F(ProtocolTest, Errors) {
  EXPECT_EQ(Run(""), "ERR empty command\n");
  EXPECT_EQ(Run("FOO a"), "ERR unknown command 'FOO'\n");
  EXPECT_EQ(Run("GET"), "ERR missing key\n");
  EXPECT_EQ(Run("GET a b"), "ERR too many arguments\n");
  EXPECT_EQ(Run("SCAN a"), "ERR usage: SCAN <start> <end|-> [limit]\n");
  EXPECT_EQ(Run("SCAN a b x"), "ERR limit must be a positive integer\n");
}

// ---------------- 网络层（真实 TCP 连接）----------------

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    db_ = DB::Open(Options(), tmp_.path());
    server_ = std::make_unique<Server>(db_.get(), 0);  // 端口 0：系统分配空闲端口
    thread_ = std::thread([this] { server_->Run(); });
  }
  void TearDown() override {
    server_->Stop();
    thread_.join();
  }

  test::TempDir tmp_;
  std::unique_ptr<DB> db_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

// Phase 7 验收：启动服务端，用客户端跑一组读写
TEST_F(ServerTest, ClientReadWrite) {
  Client client("127.0.0.1", server_->port());
  EXPECT_EQ(client.Command("SET k1 v1"), "OK\n");
  EXPECT_EQ(client.Command("SET k2 v2"), "OK\n");
  EXPECT_EQ(client.Command("GET k1"), "VALUE v1\n");
  EXPECT_EQ(client.Command("DEL k1"), "OK\n");
  EXPECT_EQ(client.Command("GET k1"), "NOT_FOUND\n");
  EXPECT_EQ(client.Command("SCAN k -"), "k2 v2\nEND\n");
  EXPECT_EQ(client.Command("NOPE"), "ERR unknown command 'NOPE'\n");
}

// 一次发出多条命令（"粘包"），以及一条命令分几次发出（"拆包"）
TEST_F(ServerTest, PipeliningAndPartialLines) {
  Client client("127.0.0.1", server_->port());
  client.SendRaw("SET a 1\nSET b 2\nGET a\n");
  EXPECT_EQ(client.ReadLine(), "OK");
  EXPECT_EQ(client.ReadLine(), "OK");
  EXPECT_EQ(client.ReadLine(), "VALUE 1");

  client.SendRaw("GE");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  client.SendRaw("T b");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  client.SendRaw("\n");
  EXPECT_EQ(client.ReadLine(), "VALUE 2");
}

TEST_F(ServerTest, ManyClientsConcurrently) {
  const int clients = 8, ops = 200;
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int c = 0; c < clients; ++c) {
    threads.emplace_back([&, c] {
      Client client("127.0.0.1", server_->port());
      for (int i = 0; i < ops; ++i) {
        std::string key = "c" + std::to_string(c) + "-" + std::to_string(i);
        if (client.Command("SET " + key + " " + std::to_string(i)) != "OK\n") ++errors;
        if (client.Command("GET " + key) != "VALUE " + std::to_string(i) + "\n") ++errors;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(db_->Scan("c", "d").size(), static_cast<size_t>(clients * ops));
}

}  // namespace
}  // namespace kv
