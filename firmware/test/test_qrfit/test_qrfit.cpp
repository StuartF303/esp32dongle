// Host-side unit tests for the QR fit decision (qrfit.h).
//
// `pio test -e native` — no hardware, no panel, no camera. What is testable
// here is everything except the scan itself:
//
//   * the geometry: modules -> pixels per module -> drawn extent -> origin,
//     including that version 4 is off the end of what an 80 px panel can show;
//   * the encode: the REAL payload strings this firmware will produce, through
//     the REAL vendored encoder, asserting the versions ARCHITECTURE.md's
//     measured table records — so if a qrcodegen update or an ECC change moves
//     a payload up a version, this suite says so rather than the panel;
//   * the fallback: a `WIFI:` payload carrying a 63-character owner-set
//     passphrase does not encode within MAX_VERSION, which is what makes the
//     renderer print text instead.
//
// NOT tested here, and not testable here: whether a phone can decode 0.27 mm
// modules off this particular sheet of glass. That is specular glare and
// ST7735 pixel bleed, and ARCHITECTURE.md says explicitly to settle it by
// scanning the real panel rather than by arithmetic.
//
// firmware/lib/qrcodegen is compiled into this env, which is the whole reason
// these assertions are against encoder output rather than against a model of
// it (see qrcodegen/PROVENANCE.md).

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "qrfit.h"

void setUp() {}
void tearDown() {}

namespace {

// The real strings, built the way the producers build them.
//
// SSID is mod_http.cpp's "tdongle-%02x%02x" with this device's own MAC tail
// (CLAUDE.md: e4:b3:23:f2:a9:d8). PIN is AuthFmt::PIN_LEN == 4 digits. The
// generated passphrase is AuthFmt::PSK_GEN_LEN == 15: 13 symbols in 4-4-5
// groups separated by '-'.
const char *SSID = "tdongle-a9d8";
const char *PIN = "4821";
const char *PSK_GENERATED = "abcd-efgh-jkmnp";

// AuthFmt::PSK_MAX == 63. Legal via `psk set`, and the row of the table that
// forced the text fallback to exist.
const char *PSK_MAX_LENGTH = "aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffggg";

void wifiPayload(char *out, size_t cap, const char *psk) {
  snprintf(out, cap, "WIFI:T:WPA;S:%s;P:%s;;", SSID, psk);
}

}  // namespace

// ---- geometry, with no encoder involved ---------------------------------

void test_module_count_per_version() {
  TEST_ASSERT_EQUAL_INT(21, QrFit::sizeForVersion(1));
  TEST_ASSERT_EQUAL_INT(25, QrFit::sizeForVersion(2));
  TEST_ASSERT_EQUAL_INT(29, QrFit::sizeForVersion(3));
  TEST_ASSERT_EQUAL_INT(33, QrFit::sizeForVersion(4));
}

void test_scale_is_two_up_to_version_three_and_nothing_above() {
  // scale = 80 / (size + 8). This is the arithmetic the whole design rests on.
  TEST_ASSERT_EQUAL_INT(2, QrFit::scaleFor(21));
  TEST_ASSERT_EQUAL_INT(2, QrFit::scaleFor(25));
  TEST_ASSERT_EQUAL_INT(2, QrFit::scaleFor(29));
  // 33 + 8 = 41, and 80/41 is 1 — below MIN_SCALE, so scaleFor reports 0
  // rather than a scale that would put 0.14 mm modules on the glass.
  TEST_ASSERT_EQUAL_INT(0, QrFit::scaleFor(33));
  TEST_ASSERT_EQUAL_INT(0, QrFit::scaleFor(37));  // version 5, the WIFI+63 case
  TEST_ASSERT_EQUAL_INT(0, QrFit::scaleFor(0));
  TEST_ASSERT_EQUAL_INT(0, QrFit::scaleFor(-1));
}

void test_extent_matches_the_measured_table() {
  // ARCHITECTURE.md §"QR pairing on the LCD": 2 px -> 58 px for version 1,
  // 2 px -> 74 px for version 3, quiet zone included in both.
  TEST_ASSERT_EQUAL_INT(58, QrFit::extentFor(21));
  TEST_ASSERT_EQUAL_INT(66, QrFit::extentFor(25));
  TEST_ASSERT_EQUAL_INT(74, QrFit::extentFor(29));
}

void test_fit_centres_the_code_in_the_block() {
  QrFit::Fit f = QrFit::fitFor(29);
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_INT(29, f.size);
  TEST_ASSERT_EQUAL_INT(2, f.scale);
  TEST_ASSERT_EQUAL_INT(74, f.extent);
  TEST_ASSERT_EQUAL_INT(3, f.offset);  // (80 - 74) / 2
  TEST_ASSERT_TRUE(f.offset * 2 + f.extent <= QrFit::BLOCK_PX);

  QrFit::Fit v1 = QrFit::fitFor(21);
  TEST_ASSERT_EQUAL_INT(58, v1.extent);
  TEST_ASSERT_EQUAL_INT(11, v1.offset);  // (80 - 58) / 2
}

void test_fit_refuses_anything_below_two_pixels_per_module() {
  QrFit::Fit f = QrFit::fitFor(37);  // version 5
  TEST_ASSERT_FALSE(f.ok);
  TEST_ASSERT_EQUAL_INT(0, f.scale);
  TEST_ASSERT_EQUAL_INT(0, f.extent);
}

// ---- the encoder, on the real payloads ----------------------------------

void test_pair_url_uppercase_scheme_is_version_one() {
  // "HTTP://192.168.4.1/4821" — 23 characters, every one of them inside QR
  // alphanumeric mode (0-9 A-Z $%*+-./: ). This is why ARCHITECTURE.md chose
  // the uppercase scheme: it is a whole version cheaper and browsers do not
  // care about scheme case.
  char url[64];
  snprintf(url, sizeof(url), "HTTP://192.168.4.1/%s", PIN);
  TEST_ASSERT_EQUAL_size_t(23, strlen(url));

  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(url, tmp, qr);
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_INT(21, f.size);  // version 1
  TEST_ASSERT_EQUAL_INT(2, f.scale);
  TEST_ASSERT_EQUAL_INT(58, f.extent);
  TEST_ASSERT_EQUAL_INT(21, qrcodegen_getSize(qr));
}

void test_lowercase_scheme_costs_a_version() {
  // The same 23 characters in lowercase fall out of alphanumeric mode into
  // byte mode and need version 2. Asserted so that anyone who "tidies" the
  // scheme to lowercase sees the cost in a failing test.
  char url[64];
  snprintf(url, sizeof(url), "http://192.168.4.1/%s", PIN);
  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(url, tmp, qr);
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_INT(25, f.size);  // version 2
  TEST_ASSERT_EQUAL_INT(66, f.extent);
}

void test_query_string_form_also_costs_a_version() {
  // '?' and '=' are not in the alphanumeric set. The path-segment route
  // (/<pin>) exists precisely to avoid them.
  const char *url = "http://192.168.4.1/?p=4821";
  TEST_ASSERT_EQUAL_size_t(26, strlen(url));
  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(url, tmp, qr);
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_INT(25, f.size);  // version 2
}

void test_join_payload_with_the_generated_passphrase_is_version_three() {
  char wifi[QrFit::BUF_LEN * 2];
  wifiPayload(wifi, sizeof(wifi), PSK_GENERATED);
  TEST_ASSERT_EQUAL_size_t(45, strlen(wifi));  // the measured table's row

  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(wifi, tmp, qr);
  TEST_ASSERT_TRUE(f.ok);
  TEST_ASSERT_EQUAL_INT(29, f.size);  // version 3 — the largest drawable
  TEST_ASSERT_EQUAL_INT(2, f.scale);
  TEST_ASSERT_EQUAL_INT(74, f.extent);
  TEST_ASSERT_EQUAL_INT(3, f.offset);
}

void test_join_payload_with_a_63_character_passphrase_does_not_encode() {
  // THE RULE THAT HAD TO BE MEASURED. 93 characters needs version 5, which is
  // above MAX_VERSION, so the encoder itself refuses — the renderer never sees
  // a symbol it would have to decline to draw.
  char wifi[QrFit::BUF_LEN * 2];
  wifiPayload(wifi, sizeof(wifi), PSK_MAX_LENGTH);
  TEST_ASSERT_EQUAL_size_t(63, strlen(PSK_MAX_LENGTH));
  TEST_ASSERT_EQUAL_size_t(93, strlen(wifi));

  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(wifi, tmp, qr);
  TEST_ASSERT_FALSE(f.ok);
  TEST_ASSERT_EQUAL_INT(0, f.size);
  TEST_ASSERT_EQUAL_INT(0, f.scale);
}

void test_encode_rejects_degenerate_input() {
  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  TEST_ASSERT_FALSE(QrFit::encode(nullptr, tmp, qr).ok);
  TEST_ASSERT_FALSE(QrFit::encode("", tmp, qr).ok);
  TEST_ASSERT_FALSE(QrFit::encode("x", nullptr, qr).ok);
  TEST_ASSERT_FALSE(QrFit::encode("x", tmp, nullptr).ok);
}

// ---- the symbol itself, so "it encoded" means something ----------------

void test_the_finder_patterns_are_where_a_decoder_expects_them() {
  // Not a decode — this suite cannot decode — but enough to prove the buffer
  // holds a real symbol rather than zeroes that happened to report a size.
  // Every QR has a 7x7 finder in three corners, each a filled ring: dark
  // border, light inner ring, 3x3 dark core.
  char url[64];
  snprintf(url, sizeof(url), "HTTP://192.168.4.1/%s", PIN);
  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(url, tmp, qr);
  TEST_ASSERT_TRUE(f.ok);

  const int n = f.size;
  const int corners[3][2] = {{0, 0}, {n - 7, 0}, {0, n - 7}};
  for (int c = 0; c < 3; c++) {
    int ox = corners[c][0];
    int oy = corners[c][1];
    TEST_ASSERT_TRUE(qrcodegen_getModule(qr, ox + 0, oy + 0));  // outer ring
    TEST_ASSERT_TRUE(qrcodegen_getModule(qr, ox + 6, oy + 0));
    TEST_ASSERT_TRUE(qrcodegen_getModule(qr, ox + 0, oy + 6));
    TEST_ASSERT_FALSE(qrcodegen_getModule(qr, ox + 1, oy + 1));  // light ring
    TEST_ASSERT_FALSE(qrcodegen_getModule(qr, ox + 5, oy + 1));
    TEST_ASSERT_TRUE(qrcodegen_getModule(qr, ox + 3, oy + 3));  // dark core
  }
  // Outside the symbol reads light, which is what lets the renderer paint the
  // quiet zone by simply querying every pixel of its block.
  TEST_ASSERT_FALSE(qrcodegen_getModule(qr, -1, 0));
  TEST_ASSERT_FALSE(qrcodegen_getModule(qr, n, n));
}

void test_a_different_pin_produces_a_different_symbol() {
  // The PIN rotates constantly (pairing.h), and every rotation must change the
  // pixels — otherwise the panel would keep showing a code for a spent PIN.
  uint8_t tmpA[QrFit::BUF_LEN], qrA[QrFit::BUF_LEN];
  uint8_t tmpB[QrFit::BUF_LEN], qrB[QrFit::BUF_LEN];
  QrFit::Fit a = QrFit::encode("HTTP://192.168.4.1/4821", tmpA, qrA);
  QrFit::Fit b = QrFit::encode("HTTP://192.168.4.1/1234", tmpB, qrB);
  TEST_ASSERT_TRUE(a.ok);
  TEST_ASSERT_TRUE(b.ok);
  TEST_ASSERT_EQUAL_INT(a.size, b.size);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(qrA, qrB, QrFit::BUF_LEN));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_module_count_per_version);
  RUN_TEST(test_scale_is_two_up_to_version_three_and_nothing_above);
  RUN_TEST(test_extent_matches_the_measured_table);
  RUN_TEST(test_fit_centres_the_code_in_the_block);
  RUN_TEST(test_fit_refuses_anything_below_two_pixels_per_module);
  RUN_TEST(test_pair_url_uppercase_scheme_is_version_one);
  RUN_TEST(test_lowercase_scheme_costs_a_version);
  RUN_TEST(test_query_string_form_also_costs_a_version);
  RUN_TEST(test_join_payload_with_the_generated_passphrase_is_version_three);
  RUN_TEST(test_join_payload_with_a_63_character_passphrase_does_not_encode);
  RUN_TEST(test_encode_rejects_degenerate_input);
  RUN_TEST(test_the_finder_patterns_are_where_a_decoder_expects_them);
  RUN_TEST(test_a_different_pin_produces_a_different_symbol);
  return UNITY_END();
}
