// Host-side unit tests for CT (ct.h).
//
// `pio test -e native` — no hardware. These assert BEHAVIOUR (does it decide
// equality correctly, does it stay in bounds); they cannot assert timing, which
// is the property the file exists for. Constant time is enforced by reading the
// code — the loop trip count is a function of `maxLen` alone — and the tests
// here exist to stop that code being "fixed" into something that early-exits
// while still passing an equality test.

#include <unity.h>

#include <string.h>

#include "ct.h"

void setUp() {}
void tearDown() {}

void test_equal_matches_memcmp_for_fixed_length() {
  TEST_ASSERT_TRUE(CT::equal("abcdefgh", "abcdefgh", 8));
  TEST_ASSERT_FALSE(CT::equal("abcdefgh", "Abcdefgh", 8));  // first byte differs
  TEST_ASSERT_FALSE(CT::equal("abcdefgh", "abcdefgH", 8));  // last byte differs
  TEST_ASSERT_TRUE(CT::equal("abcdefgh", "abcdXXXX", 4));   // only the first 4 compared
}

void test_equal_zero_length_is_trivially_equal() {
  TEST_ASSERT_TRUE(CT::equal("a", "b", 0));
}

void test_equal_rejects_null() {
  TEST_ASSERT_FALSE(CT::equal(nullptr, "a", 1));
  TEST_ASSERT_FALSE(CT::equal("a", nullptr, 1));
}

void test_equal_compares_embedded_nuls() {
  const char a[4] = {'x', '\0', 'y', 'z'};
  const char b[4] = {'x', '\0', 'y', 'Z'};
  TEST_ASSERT_FALSE(CT::equal(a, b, 4));
  TEST_ASSERT_TRUE(CT::equal(a, b, 3));
}

void test_equalstr_exact_match() {
  TEST_ASSERT_TRUE(CT::equalStr("12345678", "12345678", 8));
  TEST_ASSERT_TRUE(CT::equalStr("12345678", "12345678", 64));
}

void test_equalstr_rejects_wrong_content() {
  TEST_ASSERT_FALSE(CT::equalStr("12345678", "12345679", 64));
  TEST_ASSERT_FALSE(CT::equalStr("12345678", "02345678", 64));
}

// The whole point of folding the length mismatch into the accumulator: a
// candidate that is a PREFIX of the secret, or an extension of it, must be
// rejected — and must be rejected without a different code path.
void test_equalstr_rejects_prefix_and_extension() {
  TEST_ASSERT_FALSE(CT::equalStr("12345678", "1234567", 64));
  TEST_ASSERT_FALSE(CT::equalStr("12345678", "123456789", 64));
  TEST_ASSERT_FALSE(CT::equalStr("12345678", "", 64));
}

// maxLen bounds BOTH strings. Two strings that differ only past maxLen are
// equal as far as this function is concerned — callers pass the secret's own
// declared length, so this is the documented contract, not a bug.
void test_equalstr_is_bounded_by_maxlen() {
  TEST_ASSERT_TRUE(CT::equalStr("aaaaXXXX", "aaaaYYYY", 4));
  TEST_ASSERT_FALSE(CT::equalStr("aaaaXXXX", "aaaaYYYY", 5));
}

void test_equalstr_rejects_null() {
  TEST_ASSERT_FALSE(CT::equalStr(nullptr, "a", 4));
  TEST_ASSERT_FALSE(CT::equalStr("a", nullptr, 4));
}

// A candidate with no NUL inside maxLen must not be walked past it. Built
// without a terminator on purpose: strnlen stops at maxLen, and the loop then
// treats every position as in-range, so nothing reads off the end.
void test_equalstr_tolerates_unterminated_candidate() {
  char cand[8];
  memset(cand, 'z', sizeof(cand));
  TEST_ASSERT_FALSE(CT::equalStr("12345678", cand, 8));
  char same[8];
  memset(same, 'z', sizeof(same));
  TEST_ASSERT_TRUE(CT::equalStr(cand, same, 8));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_equal_matches_memcmp_for_fixed_length);
  RUN_TEST(test_equal_zero_length_is_trivially_equal);
  RUN_TEST(test_equal_rejects_null);
  RUN_TEST(test_equal_compares_embedded_nuls);
  RUN_TEST(test_equalstr_exact_match);
  RUN_TEST(test_equalstr_rejects_wrong_content);
  RUN_TEST(test_equalstr_rejects_prefix_and_extension);
  RUN_TEST(test_equalstr_is_bounded_by_maxlen);
  RUN_TEST(test_equalstr_rejects_null);
  RUN_TEST(test_equalstr_tolerates_unterminated_candidate);
  return UNITY_END();
}
