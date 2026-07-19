#include "lsmdb/crc32.h"

#include <gtest/gtest.h>

#include <string>

using lsmdb::Crc32;

// Known-answer test: CRC32/IEEE of the ASCII string "123456789" is the standard
// check value 0xCBF43926. Matching it proves the polynomial/reflection are right.
TEST(Crc32, CheckValue) {
  EXPECT_EQ(Crc32(std::string("123456789")), 0xCBF43926u);
}

TEST(Crc32, EmptyIsZero) {
  EXPECT_EQ(Crc32(std::string("")), 0u);
}

TEST(Crc32, DetectsSingleBitFlip) {
  std::string a = "the quick brown fox";
  std::string b = a;
  b[0] ^= 0x01;
  EXPECT_NE(Crc32(a), Crc32(b));
}
