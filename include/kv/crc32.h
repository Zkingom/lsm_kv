// crc32.h —— CRC32 校验和（IEEE 802.3 多项式，与 zlib 的 crc32 结果相同）
//
// 对应组件：LevelDB util/crc32c.h（LevelDB 用的是 Castagnoli 多项式 CRC32C，
// 现代 CPU 有 SSE4.2 指令硬件加速；这里用更常见的 IEEE 版本，原理一样）。
// 面试可能问：
//   - CRC 能防什么、不能防什么？
//     答：能高概率发现随机比特翻转、写一半的"撕裂写"。不能防恶意篡改（不是密码学哈希），
//     也不能纠错，只能发现错误。
//   - 查表法为什么快？
//     答：逐位算每个字节要 8 次移位/异或；预先算好 256 项的表后，每个字节只查一次表。
#pragma once

#include <cstdint>
#include <string_view>

namespace kv {

uint32_t Crc32(std::string_view data);

}  // namespace kv
