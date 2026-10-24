#include "kv/protocol.h"

#include <cctype>
#include <charconv>
#include <exception>

namespace kv {
namespace {

// 从 *input 头部取出一个以空格分隔的词，并跳过它后面的一个空格
std::string_view NextToken(std::string_view* input) {
  size_t end = input->find(' ');
  std::string_view token = input->substr(0, end);
  input->remove_prefix(end == std::string_view::npos ? input->size() : end + 1);
  return token;
}

std::string ToUpper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string Error(const std::string& message) { return "ERR " + message + "\n"; }

std::string DoScan(DB* db, std::string_view args) {
  std::string start(NextToken(&args));
  std::string end(NextToken(&args));
  if (start.empty() || end.empty()) return Error("usage: SCAN <start> <end|-> [limit]");
  if (end == "-") end.clear();  // 无上界

  size_t limit = kMaxScanRows;
  if (!args.empty()) {
    auto [ptr, ec] = std::from_chars(args.data(), args.data() + args.size(), limit);
    if (ec != std::errc() || ptr != args.data() + args.size() || limit == 0) {
      return Error("limit must be a positive integer");
    }
    if (limit > kMaxScanRows) limit = kMaxScanRows;
  }

  std::string response;
  for (const auto& [key, value] : db->Scan(start, end, limit)) {
    response += key;
    response += ' ';
    response += value;
    response += '\n';
  }
  return response + "END\n";
}

std::string Dispatch(DB* db, const std::string& cmd, std::string_view args) {
  if (cmd == "PING") return "PONG\n";
  if (cmd == "SCAN") return DoScan(db, args);
  if (cmd != "SET" && cmd != "GET" && cmd != "DEL") return Error("unknown command '" + cmd + "'");

  // 剩下的 SET / GET / DEL 都以一个键开头
  std::string key(NextToken(&args));
  if (key.empty()) return Error("missing key");
  if (cmd == "SET") {
    db->Put(key, std::string(args));  // 值 = 剩余部分，允许为空、允许含空格
    return "OK\n";
  }
  if (!args.empty()) return Error("too many arguments");
  if (cmd == "GET") {
    std::optional<std::string> value = db->Get(key);
    return value ? "VALUE " + *value + "\n" : "NOT_FOUND\n";
  }
  db->Delete(key);  // DEL
  return "OK\n";
}

}  // namespace

std::string ExecuteCommand(DB* db, std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);  // 兼容 telnet 发来的 \r\n
  std::string cmd = ToUpper(NextToken(&line));
  if (cmd.empty()) return Error("empty command");
  try {
    return Dispatch(db, cmd, line);
  } catch (const std::exception& e) {
    // 存储层出错（如磁盘满）只让这条命令失败，服务本身不退出
    return Error(e.what());
  }
}

}  // namespace kv
