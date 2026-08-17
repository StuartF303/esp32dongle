// Host-side unit tests for AuthFmt (authfmt.h).
//
// `pio test -e native` — no hardware, no radio. The generators take their RNG
// as a function pointer precisely so the FORMAT can be asserted here with a
// deterministic source: alphabet, length, termination, rejection sampling, and
// what happens when the RNG is broken. The quality of the real entropy source
// is mod_http.cpp's problem and is not testable from here.

#include <unity.h>

#include <math.h>
#include <string.h>

#include "authfmt.h"

using namespace AuthFmt;

// ---- deterministic RNG stubs --------------------------------------------

static uint8_t g_next = 0;
static uint8_t g_fixed = 0;

static void rngCounter(uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    out[i] = g_next++;
  }
}

static void rngFixed(uint8_t *out, size_t n) {
  memset(out, g_fixed, n);
}

void setUp() {
  g_next = 0;
  g_fixed = 0;
}
void tearDown() {}

// ---- generated PSK -------------------------------------------------------

void test_psk_shape_alphabet_and_grouping() {
  char psk[PSK_GEN_LEN + 1];
  g_next = 0;
  TEST_ASSERT_TRUE(makePsk(psk, sizeof(psk), rngCounter));
  TEST_ASSERT_EQUAL_size_t(PSK_GEN_LEN, strlen(psk));
  TEST_ASSERT_EQUAL_CHAR(PSK_GROUP_SEP, psk[4]);
  TEST_ASSERT_EQUAL_CHAR(PSK_GROUP_SEP, psk[9]);
  for (size_t i = 0; i < PSK_GEN_LEN; i++) {
    if (i == 4 || i == 9) {
      continue;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(strchr(PSK_ALPHABET, psk[i]), "PSK character outside the declared alphabet");
  }
  TEST_ASSERT_TRUE(isGeneratedPsk(psk));
  // The whole point: whatever we mint must itself be a legal WPA2 passphrase.
  TEST_ASSERT_TRUE(validPassphrase(psk));
}

// The alphabet exists to survive being read off a 160x80 screen and typed into
// a phone. If any of these ever comes back, a mistyped passphrase becomes a
// failed WPA2 association with no diagnostic.
void test_psk_alphabet_has_no_ambiguous_glyphs() {
  TEST_ASSERT_EQUAL_size_t(31, PSK_ALPHABET_LEN);
  const char *banned = "0oO1lI";
  for (const char *b = banned; *b != '\0'; b++) {
    TEST_ASSERT_NULL_MESSAGE(strchr(PSK_ALPHABET, *b), "ambiguous glyph is back in PSK_ALPHABET");
  }
  // The separator must stay out of the alphabet or isGeneratedPsk() cannot
  // tell a group boundary from a drawn symbol.
  TEST_ASSERT_NULL(strchr(PSK_ALPHABET, PSK_GROUP_SEP));
}

// 31 does not divide 256, so bytes 248..255 MUST be discarded rather than
// folded in with `% 31` — that would make the first 8 characters of the
// alphabet ~29% more likely than the rest. Feeding a counter that starts at
// 244 walks straight through the rejection window:
//   244..247 -> indices 27,28,29,30 -> w,x,y,z
//   248..255 -> DISCARDED
//   0..8     -> indices 0..8         -> 2,3,4,5,6,7,8,9,a
// grouped 4-4-5.
void test_psk_rejects_the_biasing_bytes() {
  char psk[PSK_GEN_LEN + 1];
  g_next = 244;
  TEST_ASSERT_TRUE(makePsk(psk, sizeof(psk), rngCounter));
  TEST_ASSERT_EQUAL_STRING("wxyz-2345-6789a", psk);
}

// A source stuck inside the rejection window must FAIL rather than quietly
// fall back to a biased mapping, for the same reason makePin() must.
void test_psk_fails_loudly_on_a_broken_rng() {
  char psk[PSK_GEN_LEN + 1];
  g_fixed = 251;  // >= PSK_REJECT_AT, always rejected
  TEST_ASSERT_FALSE(makePsk(psk, sizeof(psk), rngFixed));
  TEST_ASSERT_EQUAL_size_t(0, strlen(psk));
}

// 13 symbols from a 31-character alphabet is 64.4 bits. The floor that matters
// is the offline PBKDF2 attack on a captured 4-way handshake; ~60 bits is the
// stated minimum and this must not drift below it if someone shortens the
// string to make it easier to type.
void test_psk_carries_at_least_sixty_bits() {
  double bits = (double)PSK_GEN_SYMBOLS * (log((double)PSK_ALPHABET_LEN) / log(2.0));
  TEST_ASSERT_TRUE_MESSAGE(bits >= 60.0, "the generated passphrase is below the 60-bit floor");
}

void test_psk_refuses_a_short_buffer() {
  char psk[PSK_GEN_LEN];  // one short: no room for the NUL
  psk[0] = 'X';
  TEST_ASSERT_FALSE(makePsk(psk, sizeof(psk), rngCounter));
  TEST_ASSERT_EQUAL_size_t(0, strlen(psk));
}

// ---- passphrase validator ------------------------------------------------
//
// This is the gate on `http psk p:{set:"..."}`. It has to implement the WPA2
// rule and nothing else: an owner-chosen passphrase is arbitrary printable
// ASCII, so it emphatically must NOT demand the generated alphabet.

static void fillPassphrase(char *out, size_t n, char c) {
  memset(out, c, n);
  out[n] = '\0';
}

void test_passphrase_length_bounds() {
  char buf[PSK_SCAN_CAP + 8];
  size_t len = 0;

  fillPassphrase(buf, 7, 'a');
  TEST_ASSERT_EQUAL(PASSPHRASE_SHORT, checkPassphrase(buf, &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(7, len);  // the message must name the ACTUAL length

  fillPassphrase(buf, 8, 'a');
  TEST_ASSERT_EQUAL(PASSPHRASE_OK, checkPassphrase(buf, &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(8, len);

  fillPassphrase(buf, 63, 'a');
  TEST_ASSERT_EQUAL(PASSPHRASE_OK, checkPassphrase(buf, &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(63, len);

  // 64 is a raw 256-bit PMK in hex, a different WPA2 input entirely. Rejected
  // rather than half-supported.
  fillPassphrase(buf, 64, 'a');
  TEST_ASSERT_EQUAL(PASSPHRASE_LONG, checkPassphrase(buf, &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(64, len);

  // Absurdly long input is bounded by the scan cap rather than walked forever.
  fillPassphrase(buf, PSK_SCAN_CAP + 4, 'a');
  TEST_ASSERT_EQUAL(PASSPHRASE_LONG, checkPassphrase(buf, &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(PSK_SCAN_CAP, len);
}

void test_passphrase_empty_and_null() {
  size_t len = 123;
  TEST_ASSERT_EQUAL(PASSPHRASE_SHORT, checkPassphrase("", &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(0, len);
  TEST_ASSERT_EQUAL(PASSPHRASE_NULL, checkPassphrase(nullptr, &len, nullptr));
  TEST_ASSERT_EQUAL_size_t(0, len);
  TEST_ASSERT_FALSE(validPassphrase(""));
  TEST_ASSERT_FALSE(validPassphrase(nullptr));
}

void test_passphrase_rejects_control_characters() {
  size_t len = 0, bad = 0;
  TEST_ASSERT_EQUAL(PASSPHRASE_BAD_CHAR, checkPassphrase("pass\tword", &len, &bad));
  TEST_ASSERT_EQUAL_size_t(4, bad);
  TEST_ASSERT_EQUAL(PASSPHRASE_BAD_CHAR, checkPassphrase("pass\nword", &len, &bad));
  TEST_ASSERT_EQUAL_size_t(4, bad);
  // DEL, the one control character above the printable range.
  TEST_ASSERT_EQUAL(PASSPHRASE_BAD_CHAR, checkPassphrase("password\x7f", &len, &bad));
  TEST_ASSERT_EQUAL_size_t(8, bad);
}

void test_passphrase_rejects_non_ascii() {
  size_t len = 0, bad = 0;
  // "passéword" in UTF-8: 0xc3 0xa9. char is signed on both x86 and xtensa, so
  // a naive `c < 0x20` test on a plain char would ACCEPT this — the validator
  // compares through unsigned char precisely so it does not.
  TEST_ASSERT_EQUAL(PASSPHRASE_BAD_CHAR, checkPassphrase("pass\xc3\xa9word", &len, &bad));
  TEST_ASSERT_EQUAL_size_t(4, bad);
  TEST_ASSERT_EQUAL(PASSPHRASE_BAD_CHAR, checkPassphrase("password\xff", &len, &bad));
  TEST_ASSERT_EQUAL_size_t(8, bad);
}

void test_passphrase_accepts_the_printable_range() {
  // The boundaries themselves: space (0x20) and tilde (0x7e) are legal.
  TEST_ASSERT_TRUE(validPassphrase("        "));  // eight spaces
  TEST_ASSERT_TRUE(validPassphrase("~~~~~~~~"));
  TEST_ASSERT_TRUE(validPassphrase("a pass phrase with spaces"));
  TEST_ASSERT_TRUE(validPassphrase("MiXeD-CaSe_123!@#$%^&*()"));
  // The one stuart actually chose. A test, not an endorsement — see the note
  // beside psk_ in mod_http.cpp for what it costs.
  TEST_ASSERT_TRUE(validPassphrase("pass-a9d8"));
}

// The source flag in NVS is authoritative, but this must still recognise our
// own shape — and must not mistake an owner-chosen string for one of ours.
void test_is_generated_psk() {
  TEST_ASSERT_TRUE(isGeneratedPsk("wxyz-2345-6789a"));
  TEST_ASSERT_FALSE(isGeneratedPsk(nullptr));
  TEST_ASSERT_FALSE(isGeneratedPsk(""));
  TEST_ASSERT_FALSE(isGeneratedPsk("pass-a9d8"));
  TEST_ASSERT_FALSE(isGeneratedPsk("wxyz-2345-6789"));    // short
  TEST_ASSERT_FALSE(isGeneratedPsk("wxyz-2345-6789ab"));  // long
  TEST_ASSERT_FALSE(isGeneratedPsk("wxyza2345-6789a"));   // separator missing
  TEST_ASSERT_FALSE(isGeneratedPsk("wxyz-2345-6789o"));   // 'o' is not in the alphabet
}

// ---- PIN -----------------------------------------------------------------

void test_pin_is_all_digits_and_the_right_length() {
  char pin[PIN_LEN + 1];
  TEST_ASSERT_TRUE(makePin(pin, sizeof(pin), rngCounter));
  TEST_ASSERT_EQUAL_size_t(PIN_LEN, strlen(pin));
  TEST_ASSERT_TRUE(validPin(pin));
}

// The whole reason for rejection sampling: bytes 250..255 must be DISCARDED,
// not folded in with `% 10`. Feeding a counter that starts at 245 walks
// straight through the rejection window, so the expected digits are
// 245..249 -> 5,6,7,8,9, then 250..255 dropped, then 0,1,2 -> 0,1,2.
void test_pin_rejects_the_biasing_bytes() {
  char pin[PIN_LEN + 1];
  g_next = 245;
  TEST_ASSERT_TRUE(makePin(pin, sizeof(pin), rngCounter));
  TEST_ASSERT_EQUAL_STRING("56789012", pin);
}

// A source stuck on a rejected value must FAIL, not silently fall back to a
// biased mapping. A broken RNG that produces a PIN anyway is the worst outcome
// available: it looks like it worked.
void test_pin_fails_loudly_on_a_broken_rng() {
  char pin[PIN_LEN + 1];
  g_fixed = 255;
  TEST_ASSERT_FALSE(makePin(pin, sizeof(pin), rngFixed));
  TEST_ASSERT_EQUAL_size_t(0, strlen(pin));
}

void test_pin_refuses_a_short_buffer() {
  char pin[PIN_LEN];
  TEST_ASSERT_FALSE(makePin(pin, sizeof(pin), rngCounter));
  TEST_ASSERT_EQUAL_size_t(0, strlen(pin));
}

// ---- token ---------------------------------------------------------------

void test_token_is_lowercase_hex_of_the_random_bytes() {
  char tok[TOKEN_LEN + 1];
  g_fixed = 0xAB;
  TEST_ASSERT_TRUE(makeToken(tok, sizeof(tok), rngFixed));
  TEST_ASSERT_EQUAL_size_t(TOKEN_LEN, strlen(tok));
  for (size_t i = 0; i < TOKEN_LEN; i += 2) {
    TEST_ASSERT_EQUAL_CHAR('a', tok[i]);
    TEST_ASSERT_EQUAL_CHAR('b', tok[i + 1]);
  }
  TEST_ASSERT_TRUE(validToken(tok));
}

void test_token_carries_at_least_128_bits() {
  TEST_ASSERT_TRUE(TOKEN_BYTES * 8 >= 128);
}

void test_token_refuses_a_short_buffer() {
  char tok[TOKEN_LEN];
  TEST_ASSERT_FALSE(makeToken(tok, sizeof(tok), rngFixed));
  TEST_ASSERT_EQUAL_size_t(0, strlen(tok));
}

// ---- validators ----------------------------------------------------------

void test_validators_reject_the_obvious_wrongs() {
  TEST_ASSERT_FALSE(validPin(nullptr));
  TEST_ASSERT_FALSE(validPin(""));
  TEST_ASSERT_FALSE(validPin("1234567"));   // short
  TEST_ASSERT_FALSE(validPin("123456789"));  // long
  TEST_ASSERT_FALSE(validPin("1234567a"));   // not a digit
  TEST_ASSERT_TRUE(validPin("00000000"));

  TEST_ASSERT_FALSE(validToken(nullptr));
  TEST_ASSERT_FALSE(validToken(""));
  // Uppercase hex is a DIFFERENT token as far as the session table is
  // concerned, so it must be rejected rather than case-folded.
  char upper[TOKEN_LEN + 1];
  memset(upper, 'A', TOKEN_LEN);
  upper[TOKEN_LEN] = '\0';
  TEST_ASSERT_FALSE(validToken(upper));
  char nonhex[TOKEN_LEN + 1];
  memset(nonhex, 'g', TOKEN_LEN);
  nonhex[TOKEN_LEN] = '\0';
  TEST_ASSERT_FALSE(validToken(nonhex));
}

// ---- Authorization: Bearer ----------------------------------------------

static void makeHeader(char *out, size_t cap, const char *prefix, const char *tok) {
  out[0] = '\0';
  strncat(out, prefix, cap - 1);
  strncat(out, tok, cap - strlen(out) - 1);
}

void test_bearer_accepts_the_documented_forms() {
  char tok[TOKEN_LEN + 1];
  g_fixed = 0x5c;
  TEST_ASSERT_TRUE(makeToken(tok, sizeof(tok), rngFixed));

  char hdr[128];
  char got[TOKEN_LEN + 1];

  makeHeader(hdr, sizeof(hdr), "Bearer ", tok);
  TEST_ASSERT_TRUE(bearerToken(hdr, got, sizeof(got)));
  TEST_ASSERT_EQUAL_STRING(tok, got);

  makeHeader(hdr, sizeof(hdr), "bearer ", tok);  // RFC 6750: scheme is case-insensitive
  TEST_ASSERT_TRUE(bearerToken(hdr, got, sizeof(got)));
  TEST_ASSERT_EQUAL_STRING(tok, got);

  makeHeader(hdr, sizeof(hdr), "BEARER   ", tok);  // RFC 7235 allows extra SP
  TEST_ASSERT_TRUE(bearerToken(hdr, got, sizeof(got)));
  TEST_ASSERT_EQUAL_STRING(tok, got);
}

void test_bearer_rejects_everything_else() {
  char tok[TOKEN_LEN + 1];
  g_fixed = 0x5c;
  makeToken(tok, sizeof(tok), rngFixed);

  char hdr[160];
  char got[TOKEN_LEN + 1];

  TEST_ASSERT_FALSE(bearerToken(nullptr, got, sizeof(got)));
  TEST_ASSERT_FALSE(bearerToken("", got, sizeof(got)));
  TEST_ASSERT_FALSE(bearerToken("Bearer", got, sizeof(got)));
  TEST_ASSERT_FALSE(bearerToken("Bearer ", got, sizeof(got)));

  makeHeader(hdr, sizeof(hdr), "Basic ", tok);
  TEST_ASSERT_FALSE(bearerToken(hdr, got, sizeof(got)));

  // Leading whitespace, trailing junk and a second credential are all
  // rejections rather than things to salvage — this parses attacker input.
  makeHeader(hdr, sizeof(hdr), " Bearer ", tok);
  TEST_ASSERT_FALSE(bearerToken(hdr, got, sizeof(got)));

  makeHeader(hdr, sizeof(hdr), "Bearer ", tok);
  strncat(hdr, "x", sizeof(hdr) - strlen(hdr) - 1);
  TEST_ASSERT_FALSE(bearerToken(hdr, got, sizeof(got)));

  makeHeader(hdr, sizeof(hdr), "Bearer ", tok);
  strncat(hdr, ", Bearer deadbeef", sizeof(hdr) - strlen(hdr) - 1);
  TEST_ASSERT_FALSE(bearerToken(hdr, got, sizeof(got)));

  // Right length, wrong alphabet.
  char bogus[TOKEN_LEN + 1];
  memset(bogus, 'Z', TOKEN_LEN);
  bogus[TOKEN_LEN] = '\0';
  makeHeader(hdr, sizeof(hdr), "Bearer ", bogus);
  TEST_ASSERT_FALSE(bearerToken(hdr, got, sizeof(got)));
  TEST_ASSERT_EQUAL_size_t(0, strlen(got));
}

// A caller passing a buffer that cannot hold a token must be refused rather
// than handed a truncated one that would then never match anything.
void test_bearer_refuses_a_short_output_buffer() {
  char tok[TOKEN_LEN + 1];
  g_fixed = 0x11;
  makeToken(tok, sizeof(tok), rngFixed);
  char hdr[128];
  makeHeader(hdr, sizeof(hdr), "Bearer ", tok);
  char small[TOKEN_LEN];
  TEST_ASSERT_FALSE(bearerToken(hdr, small, sizeof(small)));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_psk_shape_alphabet_and_grouping);
  RUN_TEST(test_psk_alphabet_has_no_ambiguous_glyphs);
  RUN_TEST(test_psk_rejects_the_biasing_bytes);
  RUN_TEST(test_psk_fails_loudly_on_a_broken_rng);
  RUN_TEST(test_psk_carries_at_least_sixty_bits);
  RUN_TEST(test_psk_refuses_a_short_buffer);
  RUN_TEST(test_passphrase_length_bounds);
  RUN_TEST(test_passphrase_empty_and_null);
  RUN_TEST(test_passphrase_rejects_control_characters);
  RUN_TEST(test_passphrase_rejects_non_ascii);
  RUN_TEST(test_passphrase_accepts_the_printable_range);
  RUN_TEST(test_is_generated_psk);
  RUN_TEST(test_pin_is_all_digits_and_the_right_length);
  RUN_TEST(test_pin_rejects_the_biasing_bytes);
  RUN_TEST(test_pin_fails_loudly_on_a_broken_rng);
  RUN_TEST(test_pin_refuses_a_short_buffer);
  RUN_TEST(test_token_is_lowercase_hex_of_the_random_bytes);
  RUN_TEST(test_token_carries_at_least_128_bits);
  RUN_TEST(test_token_refuses_a_short_buffer);
  RUN_TEST(test_validators_reject_the_obvious_wrongs);
  RUN_TEST(test_bearer_accepts_the_documented_forms);
  RUN_TEST(test_bearer_rejects_everything_else);
  RUN_TEST(test_bearer_refuses_a_short_output_buffer);
  return UNITY_END();
}
