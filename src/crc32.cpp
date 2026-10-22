#include "kv/crc32.h"

#include <array>

namespace kv {
namespace {

// 0xEDB88320 是多项式 0x04C11DB7 的"位反转"形式。用反转形式是因为
// 这个 CRC 变体按"低位先处理"计算，这样每步只需要右移。
constexpr uint32_t kPolynomial = 0xEDB88320u;

// 编译期生成 256 项查找表：table[b] = 字节 b 单独做 8 轮逐位计算的结果
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1) ? (kPolynomial ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;  // 初值全 1：让开头的 0 字节也能影响结果
  for (char ch : data) {
    uint8_t index = static_cast<uint8_t>(crc ^ static_cast<uint8_t>(ch));
    crc = kTable[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;  // 最后取反，标准规定
}

}  // namespace kv
