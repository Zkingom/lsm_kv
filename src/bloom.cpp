#include "kv/bloom.h"

#include <algorithm>
#include <cmath>

namespace kv {

uint64_t BloomHash(std::string_view key) {
  uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a 64 位初值
  for (char c : key) {
    h ^= static_cast<uint8_t>(c);
    h *= 0x100000001b3ull;
  }
  // fmix64：让每个输入位都能影响每个输出位（雪崩效应）
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

std::string BuildBloomFilter(const std::vector<uint64_t>& key_hashes, int bits_per_key) {
  // 最优哈希个数 k = bits_per_key * ln2，限制在 [1, 30]
  int k = static_cast<int>(std::lround(bits_per_key * 0.69));
  k = std::clamp(k, 1, 30);
  // 键很少时位数组太小误判率会很高，所以至少 64 位
  size_t bits = std::max<size_t>(64, key_hashes.size() * static_cast<size_t>(bits_per_key));
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  std::string filter(bytes, '\0');
  for (uint64_t h : key_hashes) {
    uint32_t h1 = static_cast<uint32_t>(h);
    uint32_t h2 = static_cast<uint32_t>(h >> 32);
    for (int i = 0; i < k; ++i) {
      size_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bits;  // 双重哈希
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
    }
  }
  filter.push_back(static_cast<char>(k));
  return filter;
}

bool BloomMayContain(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;
  int k = static_cast<uint8_t>(filter.back());
  size_t bits = (filter.size() - 1) * 8;

  uint64_t h = BloomHash(key);
  uint32_t h1 = static_cast<uint32_t>(h);
  uint32_t h2 = static_cast<uint32_t>(h >> 32);
  for (int i = 0; i < k; ++i) {
    size_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bits;
    if ((filter[bit / 8] & (1 << (bit % 8))) == 0) return false;  // 有一位是 0：一定不存在
  }
  return true;
}

}  // namespace kv
