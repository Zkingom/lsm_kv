#include "kv/bloom.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace kv {
namespace {

std::vector<uint64_t> HashKeys(int n, const std::string& prefix) {
  std::vector<uint64_t> hashes;
  for (int i = 0; i < n; ++i) hashes.push_back(BloomHash(prefix + std::to_string(i)));
  return hashes;
}

TEST(BloomTest, EmptyFilterNeverExcludes) {
  EXPECT_TRUE(BloomMayContain("", "anything"));
}

TEST(BloomTest, NoFalseNegatives) {
  std::string filter = BuildBloomFilter(HashKeys(10000, "key"), 10);
  for (int i = 0; i < 10000; ++i) ASSERT_TRUE(BloomMayContain(filter, "key" + std::to_string(i)));
}

// 每键 10 比特，理论误判率约 0.8%；留足余量，要求低于 2%
TEST(BloomTest, FalsePositiveRateIsLow) {
  std::string filter = BuildBloomFilter(HashKeys(10000, "key"), 10);
  int false_positives = 0;
  const int probes = 100000;
  for (int i = 0; i < probes; ++i) {
    if (BloomMayContain(filter, "missing" + std::to_string(i))) ++false_positives;
  }
  double rate = static_cast<double>(false_positives) / probes;
  EXPECT_LT(rate, 0.02) << "false positive rate = " << rate;
}

}  // namespace
}  // namespace kv
