#include "lsmdb/bloom.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "lsmdb/hash.h"

using lsmdb::BloomBuilder;
using lsmdb::BloomFilter;

namespace {

std::string KeyOf(int n) {
  char buf[24];
  snprintf(buf, sizeof(buf), "present-%012d", n);
  return std::string(buf);
}
std::string MissKeyOf(int n) {
  char buf[24];
  snprintf(buf, sizeof(buf), "absent--%012d", n);  // disjoint prefix
  return std::string(buf);
}

// (1 - e^(-kn/m))^k
double TheoreticalFp(int bits_per_key, int k) {
  double ratio = static_cast<double>(k) / bits_per_key;  // k*n/m with m=bpk*n
  return std::pow(1.0 - std::exp(-ratio), k);
}

BloomFilter BuildOver(int n, int bits_per_key) {
  BloomBuilder b(bits_per_key);
  for (int i = 0; i < n; ++i) b.Add(KeyOf(i));
  return BloomFilter(b.Finish());
}

}  // namespace

// Murmur128 must be deterministic and reasonably spread across the two halves.
TEST(Hash, DeterministicAndDistinct) {
  uint64_t a[2], b[2], c[2];
  lsmdb::MurmurHash128("hello", a);
  lsmdb::MurmurHash128("hello", b);
  lsmdb::MurmurHash128("world", c);
  EXPECT_EQ(a[0], b[0]);
  EXPECT_EQ(a[1], b[1]);
  EXPECT_NE(a[0], c[0]);
  EXPECT_NE(a[0], a[1]);  // the two halves should differ
}

// A bloom filter must never report a false negative: every inserted key must
// test positive. This is the correctness guarantee reads rely on.
TEST(Bloom, NoFalseNegatives) {
  const int N = 50000;
  BloomFilter f = BuildOver(N, 10);
  for (int i = 0; i < N; ++i) {
    EXPECT_TRUE(f.MayContain(KeyOf(i))) << "false negative at " << i;
  }
}

// Measured FP rate must land near the theoretical prediction for the chosen
// bits/key. This is the number behind the "tunable false-positive rate" bullet.
TEST(Bloom, FalsePositiveRateNearTheoretical) {
  const int N = 100000;
  const int kBitsPerKey = 10;
  BloomFilter f = BuildOver(N, kBitsPerKey);

  int fp = 0;
  const int trials = 100000;
  for (int i = 0; i < trials; ++i) {
    if (f.MayContain(MissKeyOf(i))) ++fp;
  }
  double measured = static_cast<double>(fp) / trials;
  double theoretical = TheoreticalFp(kBitsPerKey, f.num_probes());

  printf("[bloom] bits/key=%d k=%d measured_fp=%.4f theoretical_fp=%.4f\n",
         kBitsPerKey, f.num_probes(), measured, theoretical);

  // Within a factor of ~1.5 of theory (finite-sample + hashing variance).
  EXPECT_GT(measured, theoretical * 0.5);
  EXPECT_LT(measured, theoretical * 1.6 + 0.002);
}

// More bits per key -> strictly fewer false positives (the tuning knob works).
TEST(Bloom, TunabilityMonotonic) {
  const int N = 80000;
  const int trials = 80000;
  auto measure = [&](int bpk) {
    BloomFilter f = BuildOver(N, bpk);
    int fp = 0;
    for (int i = 0; i < trials; ++i) {
      if (f.MayContain(MissKeyOf(i))) ++fp;
    }
    return static_cast<double>(fp) / trials;
  };
  double fp4 = measure(4);
  double fp10 = measure(10);
  double fp16 = measure(16);
  printf("[bloom] fp@4=%.4f fp@10=%.4f fp@16=%.4f\n", fp4, fp10, fp16);
  EXPECT_GT(fp4, fp10);
  EXPECT_GT(fp10, fp16);
}

TEST(Bloom, SerializeRoundTrip) {
  BloomBuilder b(10);
  for (int i = 0; i < 1000; ++i) b.Add(KeyOf(i));
  std::string bytes = b.Finish();

  BloomFilter f(bytes);
  EXPECT_TRUE(f.valid());
  for (int i = 0; i < 1000; ++i) EXPECT_TRUE(f.MayContain(KeyOf(i)));

  // Re-parsing the same bytes yields identical answers.
  BloomFilter g(bytes);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(f.MayContain(MissKeyOf(i)), g.MayContain(MissKeyOf(i)));
  }
}

TEST(Bloom, EmptyFilterAnswersMaybe) {
  // A default-constructed / absent filter must never rule a key out.
  BloomFilter f;
  EXPECT_FALSE(f.valid());
  EXPECT_TRUE(f.MayContain("anything"));
}
