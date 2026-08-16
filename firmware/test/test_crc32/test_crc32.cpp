// Host-side unit tests for Crc32 (crc32.h).
//
// `pio test -e native` — no hardware. The whole value of this number is that
// the far end can compute it independently with zlib/binascii/Go/.NET and get a
// match, so the tests are KNOWN VECTORS from outside this codebase, not
// self-consistency checks. A CRC implementation that is merely stable is
// worthless: it will happily agree with itself about a wrong answer forever.

#include <unity.h>

#include <string.h>

#include <string>

#include "crc32.h"

void setUp() {}
void tearDown() {}

static uint32_t of(const char *s) { return Crc32::compute(s, strlen(s)); }

// ---- known vectors -------------------------------------------------------

// The canonical CRC-32/ISO-HDLC check value, quoted in every catalogue of the
// algorithm. If this one fails, nothing else in this file means anything.
void test_check_value_123456789() { TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, of("123456789")); }

void test_published_vectors() {
  TEST_ASSERT_EQUAL_HEX32(0x00000000u, of(""));           // empty input
  TEST_ASSERT_EQUAL_HEX32(0xE8B7BE43u, of("a"));          // zlib.crc32(b"a")
  TEST_ASSERT_EQUAL_HEX32(0x352441C2u, of("abc"));        // zlib.crc32(b"abc")
  TEST_ASSERT_EQUAL_HEX32(0x414FA339u, of("The quick brown fox jumps over the lazy dog"));
  TEST_ASSERT_EQUAL_HEX32(0x20159D7Fu, of("message digest"));
  TEST_ASSERT_EQUAL_HEX32(0x4C2750BDu, of("abcdefghijklmnopqrstuvwxyz"));
  TEST_ASSERT_EQUAL_HEX32(0x7CA94A72u,
                          of("12345678901234567890123456789012345678901234567890123456789012345678901234567890"));
}

void test_binary_input_including_nuls() {
  const uint8_t zeros[4] = {0, 0, 0, 0};
  TEST_ASSERT_EQUAL_HEX32(0x2144DF1Cu, Crc32::compute(zeros, 4));  // zlib.crc32(b"\0\0\0\0")
  const uint8_t ff[4] = {0xff, 0xff, 0xff, 0xff};
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, Crc32::compute(ff, 4));
}

// ---- the incremental contract, which `verify` depends on -----------------

// storage.verify hashes a file in bounded slices from the module tick. If
// update()/finish() did not compose exactly like a single pass, every verify of
// a file larger than one slice would return a wrong-but-plausible value.
void test_incremental_matches_one_shot_at_every_split() {
  std::string data;
  for (int i = 0; i < 300; i++) {
    data.push_back((char)(i * 31 + 7));
  }
  uint32_t whole = Crc32::compute(data.data(), data.size());

  for (size_t split = 0; split <= data.size(); split++) {
    uint32_t c = Crc32::INIT;
    c = Crc32::update(c, data.data(), split);
    c = Crc32::update(c, data.data() + split, data.size() - split);
    if (Crc32::finish(c) != whole) {
      TEST_FAIL_MESSAGE((std::string("incremental != one-shot at split ") + std::to_string(split)).c_str());
    }
  }
}

void test_many_small_slices_match_one_shot() {
  std::string data;
  for (int i = 0; i < 1000; i++) {
    data.push_back((char)(i & 0xff));
  }
  uint32_t whole = Crc32::compute(data.data(), data.size());

  uint32_t c = Crc32::INIT;
  for (size_t i = 0; i < data.size(); i += 7) {  // deliberately not a divisor
    size_t n = data.size() - i < 7 ? data.size() - i : 7;
    c = Crc32::update(c, data.data() + i, n);
  }
  TEST_ASSERT_EQUAL_HEX32(whole, Crc32::finish(c));
}

// finish() is NOT idempotent — it is an XOR. Applying it twice returns the
// running register, which is the classic way to ship a wrong CRC that looks
// like a real one. Pinned so nobody "tidies" the API into a single call.
void test_finish_is_the_final_xor_not_an_accessor() {
  uint32_t running = Crc32::update(Crc32::INIT, "123456789", 9);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, Crc32::finish(running));
  TEST_ASSERT_EQUAL_HEX32(running, Crc32::finish(Crc32::finish(running)));
  TEST_ASSERT_TRUE(running != Crc32::finish(running));
}

void test_zero_length_and_null_are_safe() {
  TEST_ASSERT_EQUAL_HEX32(Crc32::INIT, Crc32::update(Crc32::INIT, "abc", 0));
  TEST_ASSERT_EQUAL_HEX32(Crc32::INIT, Crc32::update(Crc32::INIT, nullptr, 100));
  TEST_ASSERT_EQUAL_HEX32(0x00000000u, Crc32::compute(nullptr, 0));
}

// ---- the wire format -----------------------------------------------------

void test_hex8_is_fixed_width_lowercase_and_unprefixed() {
  char b[9];
  Crc32::toHex8(0xCBF43926u, b);
  TEST_ASSERT_EQUAL_STRING("cbf43926", b);
  Crc32::toHex8(0x00000000u, b);
  TEST_ASSERT_EQUAL_STRING("00000000", b);  // zero-padded, never "0"
  Crc32::toHex8(0x0000000Fu, b);
  TEST_ASSERT_EQUAL_STRING("0000000f", b);
  Crc32::toHex8(0xFFFFFFFFu, b);
  TEST_ASSERT_EQUAL_STRING("ffffffff", b);  // survives the top bit being set
  TEST_ASSERT_EQUAL_size_t(8, strlen(b));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_check_value_123456789);
  RUN_TEST(test_published_vectors);
  RUN_TEST(test_binary_input_including_nuls);
  RUN_TEST(test_incremental_matches_one_shot_at_every_split);
  RUN_TEST(test_many_small_slices_match_one_shot);
  RUN_TEST(test_finish_is_the_final_xor_not_an_accessor);
  RUN_TEST(test_zero_length_and_null_are_safe);
  RUN_TEST(test_hex8_is_fixed_width_lowercase_and_unprefixed);
  return UNITY_END();
}
