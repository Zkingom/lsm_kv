// protocol.h —— 文本协议：把一行命令翻译成对 DB 的调用
//
// 对应组件：类似 Redis 的 inline 命令协议（简化版）。和网络层分开，便于单独测试。
//
// 请求：一行一个命令，以 '\n' 结尾（'\r\n' 也接受）。键不能含空白；值是行尾剩余部分，可以含空格。
//   SET <key> <value>        → OK
//   GET <key>                → VALUE <value>  或  NOT_FOUND
//   DEL <key>                → OK
//   SCAN <start> <end> [n]   → 若干行 "<key> <value>"，最后一行 END
//                              区间 [start, end)；end 写 "-" 表示无上界；最多 n 行（默认且最多 1000）
//   PING                     → PONG
// 出错：ERR <原因>
//
// 面试可能问：
//   - TCP 是字节流，怎么知道一条命令结束了？（"粘包/拆包"）
//     答：协议必须自带边界。这里用换行符分隔：服务端把收到的字节攒进缓冲区，
//     每找到一个 '\n' 就切出一条完整命令；剩下的半条留着等下次数据到来。
//     另一种常见做法是"长度前缀"（Redis RESP、HTTP Content-Length）。
//   - 文本协议的缺点？
//     答：值里不能有换行；解析要逐字节找分隔符；二进制数据要转义。生产中多用长度前缀的二进制协议。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "kv/db.h"

namespace kv {

// SCAN 单次最多返回的行数：防止一条命令生成几百 MB 的响应撑爆内存
constexpr size_t kMaxScanRows = 1000;

// 执行一行命令（不含换行符），返回完整响应（每行以 '\n' 结尾）
std::string ExecuteCommand(DB* db, std::string_view line);

}  // namespace kv
