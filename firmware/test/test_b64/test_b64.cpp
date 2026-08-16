// Host-side unit tests for B64 (b64.h).
//
// `pio test -e native` — no hardware. The decoder's output goes straight onto
// the SD card, so its job is as much to REJECT as to decode: a payload that was
// corrupted in transit must fail loudly rather than be quietly repaired and
// written. Most of what follows is therefore adversarial, not happy-path.

#include <unity.h>

#include <string.h>

#include <string>

#include "b64.h"

using namespace B64;

void setUp() {}
void tearDown() {}

// ---- helpers -------------------------------------------------------------

static std::string enc(const std::string &raw) {
  char out[512];
  size_t n = encode((const uint8_t *)raw.data(), raw.size(), out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(encodedLen(raw.size()), n);
  return std::string(out, n);
}

static void rejects(const char *b64, Result expected, const char *why) {
  uint8_t out[256];
  size_t n = 12345;
  Result got = decode(b64, strlen(b64), out, sizeof(out), &n);
  if (got != expected) {
    std::string m = std::string(why) + " — \"" + b64 + "\" gave " + resultName(got) + ", expected " +
                    resultName(expected);
    TEST_FAIL_MESSAGE(m.c_str());
  }
  // A rejected input must report zero bytes, so a caller that ignores the
  // Result and trusts outLen still writes nothing.
  TEST_ASSERT_EQUAL_size_t(0, n);
}

// ---- the round trip, which is the point ---------------------------------

void test_round_trip_every_length_up_to_a_block() {
  // 0..64 bytes covers all three padding cases many times over, plus the
  // boundaries where a group straddles the end of the input.
  for (size_t len = 0; len <= 64; len++) {
    std::string raw;
    for (size_t i = 0; i < len; i++) {
      raw.push_back((char)(i * 7 + 3));  // deterministic, spans the byte range
    }
    std::string e = enc(raw);

    uint8_t back[128];
    size_t n = 0;
    Result r = decode(e.c_str(), e.size(), back, sizeof(back), &n);
    if (r != B64_OK) {
      TEST_FAIL_MESSAGE((std::string("round trip failed at length ") + std::to_string(len) + ": " + resultName(r))
                            .c_str());
    }
    TEST_ASSERT_EQUAL_size_t(len, n);
    TEST_ASSERT_EQUAL_INT(0, memcmp(raw.data(), back, len));
  }
}

void test_round_trip_covers_every_byte_value() {
  std::string raw;
  for (int i = 0; i < 256; i++) {
    raw.push_back((char)i);
  }
  char e[512];
  TEST_ASSERT_EQUAL_size_t(encodedLen(256), encode((const uint8_t *)raw.data(), 256, e, sizeof(e)));

  uint8_t back[256];
  size_t n = 0;
  TEST_ASSERT_EQUAL(B64_OK, decode(e, strlen(e), back, sizeof(back), &n));
  TEST_ASSERT_EQUAL_size_t(256, n);
  TEST_ASSERT_EQUAL_INT(0, memcmp(raw.data(), back, 256));
}

// RFC 4648 section 10, so the wire format is the one every other language's
// base64 produces, not merely self-consistent.
void test_rfc4648_vectors() {
  TEST_ASSERT_EQUAL_STRING("", enc("").c_str());
  TEST_ASSERT_EQUAL_STRING("Zg==", enc("f").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm8=", enc("fo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9v", enc("foo").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYg==", enc("foob").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYmE=", enc("fooba").c_str());
  TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", enc("foobar").c_str());
}

void test_encodes_the_standard_alphabet_not_the_url_safe_one() {
  // 0xFB 0xFF 0xBF exercises the two characters the URL-safe alphabet renames.
  const uint8_t raw[] = {0xfb, 0xff, 0xbf};
  char out[8];
  TEST_ASSERT_EQUAL_size_t(4, encode(raw, 3, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("+/+/", out);
  rejects("-_-_", B64_BAD_CHAR, "the URL-safe alphabet must not be silently accepted");
}

// ---- rejection -----------------------------------------------------------

void test_length_must_be_a_multiple_of_four() {
  rejects("Z", B64_BAD_LENGTH, "one character");
  rejects("Zm", B64_BAD_LENGTH, "unpadded 2");
  rejects("Zm9", B64_BAD_LENGTH, "unpadded 3");
  rejects("Zm9vZ", B64_BAD_LENGTH, "a whole group plus one");
}

// Every string here is 8 characters, i.e. a legal LENGTH, so the failure has to
// come from the character check rather than from the length check running first.
void test_characters_outside_the_alphabet_are_rejected() {
  rejects("Zm9v!m9v", B64_BAD_CHAR, "punctuation");
  rejects("Zm9v Zm9", B64_BAD_CHAR, "an embedded space");
  rejects("Zm9v\nZm9", B64_BAD_CHAR, "an embedded newline — MIME line wrapping is not accepted");
  rejects("Zm9v\tZm9", B64_BAD_CHAR, "a tab");
  rejects("Zm9\x80vZm9", B64_BAD_CHAR, "a high byte");
}

void test_padding_must_be_at_the_tail_of_the_last_group_only() {
  rejects("Zg==Zg==", B64_BAD_PADDING, "padding in a non-final group");
  rejects("Z===", B64_BAD_PADDING, "three pad characters");
  rejects("====", B64_BAD_PADDING, "all padding");
  rejects("=g==", B64_BAD_PADDING, "padding in position 0");
  rejects("Zg=v", B64_BAD_PADDING, "a symbol after a pad character");
}

// "AB==" is what "AA==" looks like after a one-bit transmission error. A
// lenient decoder returns 0x00 for both; this one refuses, because the whole
// contract of the storage module is that the bytes written are the bytes sent.
void test_non_canonical_tail_bits_are_rejected() {
  rejects("AB==", B64_NON_CANONICAL, "leftover bits in a 2-symbol group");
  rejects("AAB=", B64_NON_CANONICAL, "leftover bits in a 3-symbol group");
  // The canonical spellings of the same values must still decode.
  uint8_t out[8];
  size_t n = 0;
  TEST_ASSERT_EQUAL(B64_OK, decode("AA==", 4, out, sizeof(out), &n));
  TEST_ASSERT_EQUAL_size_t(1, n);
  TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
  TEST_ASSERT_EQUAL(B64_OK, decode("AAA=", 4, out, sizeof(out), &n));
  TEST_ASSERT_EQUAL_size_t(2, n);
}

// The bound check must happen BEFORE the first byte is written, or an
// over-long payload scribbles past the buffer on its way to being rejected.
void test_output_is_bounded_before_any_byte_is_written() {
  uint8_t out[8];
  memset(out, 0xAA, sizeof(out));
  size_t n = 999;
  // "Zm9vYmFyYmF6" is 12 chars -> 9 bytes, which does not fit 8.
  TEST_ASSERT_EQUAL(B64_OVERFLOW, decode("Zm9vYmFyYmF6", 12, out, 8, &n));
  TEST_ASSERT_EQUAL_size_t(0, n);
  for (size_t i = 0; i < sizeof(out); i++) {
    TEST_ASSERT_EQUAL_UINT8(0xAA, out[i]);  // untouched
  }
}

void test_encode_refuses_rather_than_truncating() {
  const uint8_t raw[] = {1, 2, 3, 4, 5, 6};
  char out[8];  // needs encodedLen(6) + 1 == 9
  memset(out, 0x7f, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, encode(raw, 6, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);  // empty, not a plausible-looking truncation
}

void test_empty_and_null_are_handled() {
  uint8_t out[4];
  size_t n = 7;
  TEST_ASSERT_EQUAL(B64_OK, decode("", 0, out, sizeof(out), &n));
  TEST_ASSERT_EQUAL_size_t(0, n);
  TEST_ASSERT_EQUAL(B64_NULL, decode(nullptr, 4, out, sizeof(out), &n));
  TEST_ASSERT_EQUAL(B64_NULL, decode("Zm9v", 4, nullptr, 8, &n));
  TEST_ASSERT_EQUAL_size_t(0, encode(nullptr, 3, (char *)out, sizeof(out)));
}

// The module advertises max_chunk from these, so they have to agree with what
// the codec actually produces.
void test_length_helpers_agree_with_the_codec() {
  TEST_ASSERT_EQUAL_size_t(0, encodedLen(0));
  TEST_ASSERT_EQUAL_size_t(4, encodedLen(1));
  TEST_ASSERT_EQUAL_size_t(4, encodedLen(3));
  TEST_ASSERT_EQUAL_size_t(8, encodedLen(4));
  TEST_ASSERT_EQUAL_size_t(2732, encodedLen(2048));  // the storage module's max_chunk
  TEST_ASSERT_EQUAL_size_t(2049, maxDecodedLen(2732));
}

void test_every_result_has_a_name_and_message() {
  for (int i = 0; i <= (int)B64_OVERFLOW; i++) {
    Result r = (Result)i;
    TEST_ASSERT_NOT_NULL(resultName(r));
    TEST_ASSERT_NOT_NULL(resultMessage(r));
    TEST_ASSERT_TRUE_MESSAGE(strcmp(resultName(r), "?") != 0, "a Result reached the wire as \"?\"");
    TEST_ASSERT_TRUE_MESSAGE(strlen(resultMessage(r)) > 1, "a Result has no human message");
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_round_trip_every_length_up_to_a_block);
  RUN_TEST(test_round_trip_covers_every_byte_value);
  RUN_TEST(test_rfc4648_vectors);
  RUN_TEST(test_encodes_the_standard_alphabet_not_the_url_safe_one);
  RUN_TEST(test_length_must_be_a_multiple_of_four);
  RUN_TEST(test_characters_outside_the_alphabet_are_rejected);
  RUN_TEST(test_padding_must_be_at_the_tail_of_the_last_group_only);
  RUN_TEST(test_non_canonical_tail_bits_are_rejected);
  RUN_TEST(test_output_is_bounded_before_any_byte_is_written);
  RUN_TEST(test_encode_refuses_rather_than_truncating);
  RUN_TEST(test_empty_and_null_are_handled);
  RUN_TEST(test_length_helpers_agree_with_the_codec);
  RUN_TEST(test_every_result_has_a_name_and_message);
  return UNITY_END();
}
