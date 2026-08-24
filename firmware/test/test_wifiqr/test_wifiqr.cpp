// Host-side unit tests for the `WIFI:` join payload builder (wifiqr.h).
//
// `pio test -e native`. Three things are covered here and each of them is
// unreachable from the device on this machine:
//
//   * THE ESCAPING. All five `WIFI:` metacharacters (\ ; , : ") in both the S
//     and the P field, individually and together. The generated passphrase
//     contains none of them, but `psk set` accepts any printable ASCII
//     0x20..0x7e — so this is a supported input, not a hypothetical one.
//   * THE REFUSAL. An over-length result returns false and leaves `out` empty
//     rather than truncating. A truncated `WIFI:` payload still encodes and
//     still scans; it just carries the wrong passphrase.
//   * THE HANDOFF TO qrfit.h. The builder's own output is fed straight into
//     QrFit::encode(), so the two rows of ARCHITECTURE.md's measured table
//     that involve a join code are asserted against the REAL encoder from the
//     REAL producer, rather than against a hand-typed string:
//         generated 15-char PSK -> 45 chars -> version 3, drawable
//         owner-set 63-char PSK -> 93 chars -> version 5, NOT drawable
//     The second is the case that exercises the text fallback, which B1
//     reports has never run on hardware.
//
// NOT covered here, and not coverable here: whether a phone's camera actually
// joins the network from the symbol. That needs an 802.11 PHY and a camera,
// and this machine has neither (/sys/class/ieee80211 does not exist).

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "qrfit.h"
#include "wifiqr.h"

void setUp() {}
void tearDown() {}

namespace {

// The real values. SSID is mod_http.cpp's "tdongle-%02x%02x" with this
// device's MAC tail (CLAUDE.md: e4:b3:23:f2:a9:d8); the generated passphrase
// is AuthFmt::PSK_GEN_LEN == 15 in 4-4-5 groups.
const char *SSID = "tdongle-a9d8";
const char *PSK_GENERATED = "abcd-efgh-jkmnp";

// AuthFmt::PSK_MAX == 63. Legal via `psk set`.
const char *PSK_MAX_LENGTH = "aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffggg";

// pairing.h's buffer, restated here rather than included: this suite is about
// whether the two files AGREE on the bound, and importing the constant would
// make that agreement true by construction instead of by test.
constexpr size_t MAX_PAYLOAD = 224;

}  // namespace

// ---- the plain case ------------------------------------------------------

void test_generated_form_is_byte_for_byte_what_the_table_measured() {
  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), SSID, PSK_GENERATED));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:tdongle-a9d8;P:abcd-efgh-jkmnp;;", out);
  // ARCHITECTURE.md §"QR pairing on the LCD", row 4: 45 characters.
  TEST_ASSERT_EQUAL_size_t(45, strlen(out));
}

void test_joinLen_agrees_with_what_join_actually_wrote() {
  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), SSID, PSK_GENERATED));
  TEST_ASSERT_EQUAL_size_t(WifiQr::joinLen(SSID, PSK_GENERATED), strlen(out));
}

void test_envelope_lengths_are_what_the_arithmetic_assumes() {
  TEST_ASSERT_EQUAL_size_t(13, strlen(WifiQr::PREFIX));
  TEST_ASSERT_EQUAL_size_t(3, strlen(WifiQr::MID));
  TEST_ASSERT_EQUAL_size_t(2, strlen(WifiQr::SUFFIX));
  TEST_ASSERT_EQUAL_size_t(18, WifiQr::FIXED_LEN);
}

// ---- the escaping --------------------------------------------------------

void test_the_escape_set_is_exactly_the_five_metacharacters() {
  TEST_ASSERT_TRUE(WifiQr::needsEscape('\\'));
  TEST_ASSERT_TRUE(WifiQr::needsEscape(';'));
  TEST_ASSERT_TRUE(WifiQr::needsEscape(','));
  TEST_ASSERT_TRUE(WifiQr::needsEscape(':'));
  TEST_ASSERT_TRUE(WifiQr::needsEscape('"'));
  // Everything else in the printable range is passed through. `psk set`
  // accepts 0x20..0x7e, so this loop covers its whole input alphabet.
  for (int c = 0x20; c <= 0x7e; c++) {
    char ch = (char)c;
    bool special = ch == '\\' || ch == ';' || ch == ',' || ch == ':' || ch == '"';
    TEST_ASSERT_EQUAL_INT(special ? 1 : 0, WifiQr::needsEscape(ch) ? 1 : 0);
  }
}

void test_semicolon_in_the_passphrase_is_escaped() {
  // The failure this exists to prevent: unescaped, a phone parses `pass` as
  // the whole P value and the association fails with no message anywhere.
  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), SSID, "pass;word"));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:tdongle-a9d8;P:pass\\;word;;", out);
}

void test_every_metacharacter_in_the_passphrase() {
  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), "net", "a\\b;c,d:e\"f"));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:net;P:a\\\\b\\;c\\,d\\:e\\\"f;;", out);
}

void test_metacharacters_in_the_ssid_too() {
  // Not reachable from buildSsid() — "tdongle-%02x%02x" cannot produce one —
  // but the S field has the same grammar and is escaped by the same code, and
  // a future SSID source must not have to rediscover that.
  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), "my;net", "key"));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:my\\;net;P:key;;", out);
}

void test_escapedLen_counts_two_for_each_metacharacter() {
  TEST_ASSERT_EQUAL_size_t(0, WifiQr::escapedLen(nullptr));
  TEST_ASSERT_EQUAL_size_t(0, WifiQr::escapedLen(""));
  TEST_ASSERT_EQUAL_size_t(4, WifiQr::escapedLen("abcd"));
  TEST_ASSERT_EQUAL_size_t(2, WifiQr::escapedLen(";"));
  TEST_ASSERT_EQUAL_size_t(10, WifiQr::escapedLen("\\;,:\""));
}

void test_a_passphrase_of_nothing_but_semicolons_is_the_worst_legal_case() {
  // 63 semicolons is a legal `psk set` value (printable ASCII, 8..63) and the
  // most expensive one there is: every byte doubles.
  char psk[64];
  memset(psk, ';', 63);
  psk[63] = '\0';
  TEST_ASSERT_EQUAL_size_t(126, WifiQr::escapedLen(psk));

  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), SSID, psk));
  // 18 envelope + 12 ssid + 126 escaped psk.
  TEST_ASSERT_EQUAL_size_t(156, strlen(out));
  TEST_ASSERT_TRUE(strlen(out) <= MAX_PAYLOAD);
}

void test_the_worst_legal_case_of_all_still_fits_MAX_PAYLOAD() {
  // pairing.h's stated worst case: a 23-character SSID (ssid_ is char[24])
  // and a 63-character passphrase, both entirely metacharacters. 13 + 46 + 3
  // + 126 + 2 = 190. This is the assertion that keeps MAX_PAYLOAD's 224 and
  // wifiqr.h's arithmetic from drifting apart.
  char ssid[24];
  memset(ssid, ':', 23);
  ssid[23] = '\0';
  char psk[64];
  memset(psk, ':', 63);
  psk[63] = '\0';
  TEST_ASSERT_EQUAL_size_t(190, WifiQr::joinLen(ssid, psk));

  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), ssid, psk));
  TEST_ASSERT_EQUAL_size_t(190, strlen(out));
}

// ---- the refusal ---------------------------------------------------------

void test_over_length_refuses_and_leaves_out_empty() {
  // Deliberately beyond anything a legal SSID/passphrase pair can produce:
  // the bound is enforced on the LENGTH, not on the legality of the inputs,
  // so a future producer that hands this something long gets a refusal rather
  // than a payload that scans as the wrong network.
  char ssid[200];
  memset(ssid, 'x', sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';

  char out[MAX_PAYLOAD + 1];
  memset(out, 'Z', sizeof(out));
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), ssid, PSK_MAX_LENGTH));
  // EMPTY, not truncated: mod_http.cpp publishes no payload on false and the
  // panel prints the passphrase as text instead.
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_the_refusal_boundary_is_exact_at_MAX_PAYLOAD() {
  // Build an SSID that makes the payload land on exactly MAX_PAYLOAD, then
  // one byte more. 18 envelope + strlen(psk) 15 -> ssid must be 191 to hit
  // 224 exactly.
  const size_t fill = MAX_PAYLOAD - WifiQr::FIXED_LEN - strlen(PSK_GENERATED);
  TEST_ASSERT_EQUAL_size_t(191, fill);

  char ssid[256];
  memset(ssid, 'x', fill);
  ssid[fill] = '\0';

  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(out, sizeof(out), ssid, PSK_GENERATED));
  TEST_ASSERT_EQUAL_size_t(MAX_PAYLOAD, strlen(out));

  ssid[fill] = 'x';
  ssid[fill + 1] = '\0';
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), ssid, PSK_GENERATED));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_a_buffer_one_byte_short_is_refused_not_clipped() {
  char out[45];  // the generated form is 45 characters + NUL == 46
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), SSID, PSK_GENERATED));
  TEST_ASSERT_EQUAL_STRING("", out);

  char big[46];
  TEST_ASSERT_TRUE(WifiQr::join(big, sizeof(big), SSID, PSK_GENERATED));
  TEST_ASSERT_EQUAL_size_t(45, strlen(big));
}

void test_missing_or_empty_inputs_are_refused() {
  char out[MAX_PAYLOAD + 1];
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), nullptr, PSK_GENERATED));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), "", PSK_GENERATED));
  TEST_ASSERT_EQUAL_STRING("", out);
  // An empty passphrase is NOT rendered as an open network: `T:WPA` with a
  // blank `P:` would claim a security posture the device does not have.
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), SSID, nullptr));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(WifiQr::join(out, sizeof(out), SSID, ""));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_null_buffer_and_zero_cap_are_tolerated() {
  TEST_ASSERT_FALSE(WifiQr::join(nullptr, 32, SSID, PSK_GENERATED));
  char out[1];
  out[0] = 'Z';
  TEST_ASSERT_FALSE(WifiQr::join(out, 0, SSID, PSK_GENERATED));
  TEST_ASSERT_EQUAL_CHAR('Z', out[0]);  // cap 0 means "not one byte to write"
}

// ---- the handoff to qrfit.h ---------------------------------------------

void test_generated_form_encodes_at_version_three_and_is_drawable() {
  char payload[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(payload, sizeof(payload), SSID, PSK_GENERATED));

  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(payload, tmp, qr);
  TEST_ASSERT_TRUE(f.ok);
  // ARCHITECTURE.md's table: version 3, 29 modules, 2 px/module, 74 px drawn.
  TEST_ASSERT_EQUAL_INT(29, f.size);
  TEST_ASSERT_EQUAL_INT(2, f.scale);
  TEST_ASSERT_EQUAL_INT(74, f.extent);
}

void test_a_63_character_passphrase_is_built_but_will_not_draw() {
  // THE FALLBACK CASE, END TO END. The builder succeeds — the payload fits
  // MAX_PAYLOAD comfortably at 93 bytes — and qrfit.h then declines it,
  // because version 5 at 80 px of panel is 1 px per module. That is the split
  // the two files are meant to have: wifiqr.h answers "can I express this",
  // qrfit.h answers "can it be read off the glass", and only the second says
  // no here.
  char payload[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(payload, sizeof(payload), SSID, PSK_MAX_LENGTH));
  // ARCHITECTURE.md's table, last row: 93 characters.
  TEST_ASSERT_EQUAL_size_t(93, strlen(payload));

  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(payload, tmp, qr);
  TEST_ASSERT_FALSE(f.ok);
  TEST_ASSERT_EQUAL_INT(0, f.scale);
  // The renderer draws s.fallback instead, which mod_http.cpp sets to the
  // passphrase alone — 63 characters, which is exactly what
  // Pairing::MAX_FALLBACK (64) is sized for.
  TEST_ASSERT_EQUAL_size_t(63, strlen(PSK_MAX_LENGTH));
}

void test_an_escaped_passphrase_can_push_a_shorter_key_off_the_panel_too() {
  // Not only length: escaping is a cost the user cannot see. A 40-character
  // passphrase of semicolons occupies 80 bytes in the payload and lands well
  // past what version 3 can hold, so it takes the text fallback despite being
  // 23 characters SHORTER than the one above. Worth asserting because the
  // obvious mental model ("under 63 is fine") is wrong.
  char psk[41];
  memset(psk, ';', 40);
  psk[40] = '\0';

  char payload[MAX_PAYLOAD + 1];
  TEST_ASSERT_TRUE(WifiQr::join(payload, sizeof(payload), SSID, psk));
  TEST_ASSERT_EQUAL_size_t(110, strlen(payload));  // 18 + 12 + 80

  uint8_t tmp[QrFit::BUF_LEN];
  uint8_t qr[QrFit::BUF_LEN];
  QrFit::Fit f = QrFit::encode(payload, tmp, qr);
  TEST_ASSERT_FALSE(f.ok);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_generated_form_is_byte_for_byte_what_the_table_measured);
  RUN_TEST(test_joinLen_agrees_with_what_join_actually_wrote);
  RUN_TEST(test_envelope_lengths_are_what_the_arithmetic_assumes);

  RUN_TEST(test_the_escape_set_is_exactly_the_five_metacharacters);
  RUN_TEST(test_semicolon_in_the_passphrase_is_escaped);
  RUN_TEST(test_every_metacharacter_in_the_passphrase);
  RUN_TEST(test_metacharacters_in_the_ssid_too);
  RUN_TEST(test_escapedLen_counts_two_for_each_metacharacter);
  RUN_TEST(test_a_passphrase_of_nothing_but_semicolons_is_the_worst_legal_case);
  RUN_TEST(test_the_worst_legal_case_of_all_still_fits_MAX_PAYLOAD);

  RUN_TEST(test_over_length_refuses_and_leaves_out_empty);
  RUN_TEST(test_the_refusal_boundary_is_exact_at_MAX_PAYLOAD);
  RUN_TEST(test_a_buffer_one_byte_short_is_refused_not_clipped);
  RUN_TEST(test_missing_or_empty_inputs_are_refused);
  RUN_TEST(test_null_buffer_and_zero_cap_are_tolerated);

  RUN_TEST(test_generated_form_encodes_at_version_three_and_is_drawable);
  RUN_TEST(test_a_63_character_passphrase_is_built_but_will_not_draw);
  RUN_TEST(test_an_escaped_passphrase_can_push_a_shorter_key_off_the_panel_too);
  return UNITY_END();
}
