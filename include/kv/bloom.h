// bloom.h —— 布隆过滤器
//
// 对应组件：LevelDB util/bloom.cc（FilterPolicy）。
// 原理：m 个比特位，k 个哈希函数。插入时把 k 个位置置 1；查询时 k 个位置全为 1 才"可能存在"，
// 只要有一个是 0 就"一定不存在"。所以只有假阳性（误判存在），没有假阴性。
//
// 面试可能问：
//   - 误判率怎么算？每个键 10 比特时多少？
//     答：p ≈ (1 - e^(-k·n/m))^k，最优 k = (m/n)·ln2。m/n=10 时 k≈7，p≈0.8%。
//   - 为什么不用 k 个独立哈希函数？
//     答：用"双重哈希" h_i = h1 + i·h2 模拟 k 个哈希，效果几乎一样，只需算一次 64 位哈希
//     （Kirsch & Mitzenmacher 2006 的结论）。
//   - 布隆过滤器对范围查询有用吗？
//     答：没用。它只能回答"某个确定的键在不在"。范围查询要靠 SSTable 的最小/最大键做裁剪
//     （RocksDB 有前缀布隆过滤器可以部分解决）。
//   - 为什么对 LSM-tree 特别重要？
//     答：查一个不存在的键，理论上要查遍所有层的所有可能文件（读放大）。
//     过滤器让大多数文件不用读数据块就能跳过。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kv {

// 64 位哈希（FNV-1a + MurmurHash3 的 fmix64 终结混淆，让相似的键哈希值也分散）
uint64_t BloomHash(std::string_view key);

// 由所有键的哈希值构造过滤器。格式：[比特数组][k: 1 字节]
std::string BuildBloomFilter(const std::vector<uint64_t>& key_hashes, int bits_per_key);

// 空过滤器（长度为 0）一律返回 true：表示"没有过滤器，不能排除"
bool BloomMayContain(std::string_view filter, std::string_view key);

}  // namespace kv
