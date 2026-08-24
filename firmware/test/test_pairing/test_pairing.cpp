// Host-side unit tests for the pairing rendezvous (pairing.h).
//
// `pio test -e native` — no hardware, no radio, no panel. The reason this has
// tests at all is that it is the one place SECRETS cross a module boundary,
// and every property that makes that acceptable is testable without hardware:
//
//   * the policy (shouldShow) is exactly "AP up AND nothing paired", including
//     that it comes BACK when the last session goes away;
//   * nothing is stored unless a consumer subscribed, so with the LCD off the
//     PIN and the passphrase-bearing JOIN payload exist in mod_http's buffers
//     and nowhere else;
//   * unsubscribing and withdraw() actually wipe every buffer, not just the
//     PIN — the QR payload is a secret by the same rules (see the header);
//   * the record moves ATOMICALLY: code, PIN and payload are published and read
//     together, so nothing can observe CODE_PAIR beside a `WIFI:` payload;
//   * an over-length payload is REFUSED rather than truncated, because a
//     truncated QR still scans and lies.

#include <unity.h>

#include <string.h>

#include "pairing.h"

void setUp() { Pairing::subscribe(false); }
void tearDown() { Pairing::subscribe(false); }

namespace {

// Representative real payloads. ARCHITECTURE.md §"QR pairing on the LCD".
const char *PAIR_URL = "HTTP://192.168.4.1/4821";
const char *JOIN_WIFI = "WIFI:T:WPA;S:tdongle-a9d8;P:abcd-efgh-jkmnp;;";
const char *JOIN_FALLBACK = "abcd-efgh-jkmnp";

}  // namespace

// ---- the policy ---------------------------------------------------------

void test_policy_shows_only_while_unpaired_and_up() {
  TEST_ASSERT_TRUE(Pairing::shouldShow(true, 0));    // AP up, nobody paired
  TEST_ASSERT_FALSE(Pairing::shouldShow(true, 1));   // someone paired
  // Unreachable through mod_http.cpp since 2026-08-24 (MAX_SESSIONS is 1), but
  // asserted anyway: the predicate takes a COUNT and must stay correct for one,
  // rather than quietly becoming a boolean that happens to work.
  TEST_ASSERT_FALSE(Pairing::shouldShow(true, 4));   // several paired
  TEST_ASSERT_FALSE(Pairing::shouldShow(false, 0));  // AP down
  TEST_ASSERT_FALSE(Pairing::shouldShow(false, 1));
}

void test_policy_returns_when_the_last_session_goes_away() {
  // The device is unpaired again, so pairing has to be possible again. This
  // is a deliberate consequence of the rule, not an accident of it.
  TEST_ASSERT_FALSE(Pairing::shouldShow(true, 1));
  TEST_ASSERT_TRUE(Pairing::shouldShow(true, 0));
}

// ---- storing nothing when nobody is looking -----------------------------

void test_publish_stores_nothing_while_unsubscribed() {
  Pairing::Snapshot s;
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_NONE, Pairing::code());
  TEST_ASSERT_FALSE(Pairing::get(s));
  TEST_ASSERT_EQUAL_STRING("", s.pin);
  TEST_ASSERT_EQUAL_STRING("", s.payload);
}

void test_the_join_payload_is_not_stored_while_unsubscribed_either() {
  // The passphrase is in that string. With the LCD off it must not reach this
  // translation unit at all.
  Pairing::Snapshot s;
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  TEST_ASSERT_FALSE(Pairing::get(s));
  TEST_ASSERT_EQUAL_STRING("", s.payload);
  TEST_ASSERT_EQUAL_STRING("", s.fallback);
}

void test_unsubscribe_wipes_every_buffer() {
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  TEST_ASSERT_TRUE(Pairing::visible());

  Pairing::subscribe(false);
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_NONE, Pairing::code());
  TEST_ASSERT_FALSE(Pairing::get(s));
  TEST_ASSERT_EQUAL_STRING("", s.pin);
  TEST_ASSERT_EQUAL_STRING("", s.payload);
  TEST_ASSERT_EQUAL_STRING("", s.fallback);
}

// ---- publish / get, as one record ---------------------------------------

void test_publish_and_get_a_pair_record() {
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  TEST_ASSERT_TRUE(Pairing::visible());
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_PAIR, Pairing::code());
  TEST_ASSERT_TRUE(Pairing::get(s));
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_PAIR, s.code);
  TEST_ASSERT_EQUAL_STRING("4821", s.pin);
  TEST_ASSERT_EQUAL_STRING(PAIR_URL, s.payload);
  TEST_ASSERT_EQUAL_STRING("", s.fallback);
}

void test_publish_and_get_a_join_record() {
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  TEST_ASSERT_TRUE(Pairing::get(s));
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_JOIN, s.code);
  TEST_ASSERT_EQUAL_STRING("4821", s.pin);
  TEST_ASSERT_EQUAL_STRING(JOIN_WIFI, s.payload);
  TEST_ASSERT_EQUAL_STRING(JOIN_FALLBACK, s.fallback);
}

void test_the_record_changes_all_at_once() {
  // The producer swaps JOIN for PAIR the moment a station associates. A reader
  // must never see the new code beside the old payload — get() copies the whole
  // record, so this is the assertion that the code and payload agree.
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  TEST_ASSERT_TRUE(Pairing::get(s));
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_PAIR, s.code);
  TEST_ASSERT_EQUAL_STRING(PAIR_URL, s.payload);
  // And the JOIN payload — which carries the passphrase — is gone from the
  // buffer, not left as a longer string with a shorter one written over it.
  TEST_ASSERT_EQUAL_STRING("", s.fallback);
  TEST_ASSERT_NULL(strstr(s.payload, "WIFI:"));
}

void test_a_payload_is_optional() {
  // The PIN with no QR beside it is a legitimate state, not an error: it is
  // what a producer publishes before it has an IP, and what pass B2 would
  // publish if it ever chose to.
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_PAIR, "4821", nullptr, nullptr);
  TEST_ASSERT_TRUE(Pairing::get(s));
  TEST_ASSERT_EQUAL_STRING("4821", s.pin);
  TEST_ASSERT_EQUAL_STRING("", s.payload);
}

void test_withdraw_clears() {
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  Pairing::withdraw();
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_NONE, Pairing::code());
  TEST_ASSERT_FALSE(Pairing::get(s));
  TEST_ASSERT_EQUAL_STRING("", s.payload);
}

void test_publish_null_or_empty_pin_withdraws() {
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  Pairing::publish(Pairing::CODE_PAIR, nullptr, PAIR_URL, "");
  TEST_ASSERT_FALSE(Pairing::visible());

  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  Pairing::publish(Pairing::CODE_PAIR, "", PAIR_URL, "");
  TEST_ASSERT_FALSE(Pairing::visible());
}

void test_publish_code_none_withdraws() {
  // Withdrawing by code, so the producer's "nothing to show" branch does not
  // have to reach for a different function.
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  TEST_ASSERT_TRUE(Pairing::visible());
  Pairing::publish(Pairing::CODE_NONE, "4821", PAIR_URL, "");
  TEST_ASSERT_FALSE(Pairing::visible());
}

// ---- seq(), which is what drives the repaint ----------------------------

void test_republishing_the_same_record_does_not_move_seq() {
  // mod_http calls publish() from a 250 ms tick. If an unchanged value moved
  // the sequence counter the LCD would re-encode and repaint a 74x74 QR four
  // times a second forever.
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  uint32_t after = Pairing::seq();
  for (int i = 0; i < 20; i++) {
    Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, JOIN_FALLBACK);
  }
  TEST_ASSERT_EQUAL_UINT32(after, Pairing::seq());
}

void test_seq_moves_when_any_field_changes() {
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  uint32_t a = Pairing::seq();

  // A new PIN. The URL carries it too, so both fields move together in real
  // use — but each alone must be enough.
  Pairing::publish(Pairing::CODE_PAIR, "1234", PAIR_URL, "");
  uint32_t b = Pairing::seq();
  TEST_ASSERT_NOT_EQUAL(a, b);

  // Same PIN, different payload.
  Pairing::publish(Pairing::CODE_PAIR, "1234", "HTTP://192.168.4.1/1234", "");
  uint32_t c = Pairing::seq();
  TEST_ASSERT_NOT_EQUAL(b, c);

  // Same PIN and payload, different code — the JOIN/PAIR switch on its own.
  Pairing::publish(Pairing::CODE_JOIN, "1234", "HTTP://192.168.4.1/1234", "");
  uint32_t d = Pairing::seq();
  TEST_ASSERT_NOT_EQUAL(c, d);

  // Same everything else, different fallback text.
  Pairing::publish(Pairing::CODE_JOIN, "1234", "HTTP://192.168.4.1/1234", "abcd-efgh-jkmnp");
  TEST_ASSERT_NOT_EQUAL(d, Pairing::seq());

  Pairing::withdraw();
  TEST_ASSERT_NOT_EQUAL(d, Pairing::seq());
}

void test_withdraw_when_already_clear_does_not_move_seq() {
  Pairing::subscribe(true);
  uint32_t a = Pairing::seq();
  Pairing::withdraw();
  TEST_ASSERT_EQUAL_UINT32(a, Pairing::seq());
}

// ---- bounds -------------------------------------------------------------

void test_publish_bounds_an_overlong_pin() {
  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_PAIR, "0123456789abcdef0123456789abcdef", "", "");  // 32, twice MAX_PIN
  TEST_ASSERT_TRUE(Pairing::get(s));
  TEST_ASSERT_EQUAL_size_t(Pairing::MAX_PIN, strlen(s.pin));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef", s.pin);
}

void test_an_overlong_payload_is_refused_not_truncated() {
  // THE ASYMMETRY, ASSERTED. A truncated `WIFI:` payload still encodes and
  // still scans — it hands the phone a wrong passphrase that looks right. So
  // the whole record is refused instead.
  char big[Pairing::MAX_PAYLOAD + 8];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", big, "");
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_FALSE(Pairing::get(s));

  // And it takes an already-good record down with it rather than leaving a
  // stale QR beside a fresh PIN.
  Pairing::publish(Pairing::CODE_PAIR, "4821", PAIR_URL, "");
  TEST_ASSERT_TRUE(Pairing::visible());
  Pairing::publish(Pairing::CODE_JOIN, "4821", big, "");
  TEST_ASSERT_FALSE(Pairing::visible());
}

void test_an_overlong_fallback_is_refused_too() {
  char big[Pairing::MAX_FALLBACK + 8];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", JOIN_WIFI, big);
  TEST_ASSERT_FALSE(Pairing::visible());
}

void test_the_longest_legal_payload_still_fits() {
  // 23-character SSID and a 63-character WPA2 passphrase, both fully
  // backslash-escaped: the worst case MAX_PAYLOAD was sized for. It is refused
  // by the ENCODER later (qrfit.h) and drawn as text — but it must reach the
  // renderer intact to be drawn as anything at all.
  char escaped[Pairing::MAX_PAYLOAD + 1];
  size_t n = 0;
  const char *prefix = "WIFI:T:WPA;S:";
  for (const char *p = prefix; *p; p++) {
    escaped[n++] = *p;
  }
  for (int i = 0; i < 23; i++) {  // SSID, worst case escaped to 2 bytes each
    escaped[n++] = '\\';
    escaped[n++] = ';';
  }
  escaped[n++] = ';';
  escaped[n++] = 'P';
  escaped[n++] = ':';
  for (int i = 0; i < 63; i++) {
    escaped[n++] = '\\';
    escaped[n++] = ';';
  }
  escaped[n++] = ';';
  escaped[n++] = ';';
  escaped[n] = '\0';
  TEST_ASSERT_EQUAL_size_t(190, n);
  TEST_ASSERT_TRUE(n <= Pairing::MAX_PAYLOAD);

  Pairing::Snapshot s;
  Pairing::subscribe(true);
  Pairing::publish(Pairing::CODE_JOIN, "4821", escaped, "");
  TEST_ASSERT_TRUE(Pairing::get(s));
  TEST_ASSERT_EQUAL_STRING(escaped, s.payload);
}

void test_get_zeroes_the_snapshot_when_nothing_is_published() {
  // The caller keeps this on the stack and memsets it on the way out. It must
  // arrive clean too, so a failed get() cannot leave a previous record's
  // passphrase visible through an uninitialised local.
  Pairing::Snapshot s;
  memset(&s, 'x', sizeof(s));
  TEST_ASSERT_FALSE(Pairing::get(s));
  TEST_ASSERT_EQUAL_UINT8(Pairing::CODE_NONE, s.code);
  TEST_ASSERT_EQUAL_STRING("", s.pin);
  TEST_ASSERT_EQUAL_STRING("", s.payload);
  TEST_ASSERT_EQUAL_STRING("", s.fallback);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_policy_shows_only_while_unpaired_and_up);
  RUN_TEST(test_policy_returns_when_the_last_session_goes_away);
  RUN_TEST(test_publish_stores_nothing_while_unsubscribed);
  RUN_TEST(test_the_join_payload_is_not_stored_while_unsubscribed_either);
  RUN_TEST(test_unsubscribe_wipes_every_buffer);
  RUN_TEST(test_publish_and_get_a_pair_record);
  RUN_TEST(test_publish_and_get_a_join_record);
  RUN_TEST(test_the_record_changes_all_at_once);
  RUN_TEST(test_a_payload_is_optional);
  RUN_TEST(test_withdraw_clears);
  RUN_TEST(test_publish_null_or_empty_pin_withdraws);
  RUN_TEST(test_publish_code_none_withdraws);
  RUN_TEST(test_republishing_the_same_record_does_not_move_seq);
  RUN_TEST(test_seq_moves_when_any_field_changes);
  RUN_TEST(test_withdraw_when_already_clear_does_not_move_seq);
  RUN_TEST(test_publish_bounds_an_overlong_pin);
  RUN_TEST(test_an_overlong_payload_is_refused_not_truncated);
  RUN_TEST(test_an_overlong_fallback_is_refused_too);
  RUN_TEST(test_the_longest_legal_payload_still_fits);
  RUN_TEST(test_get_zeroes_the_snapshot_when_nothing_is_published);
  return UNITY_END();
}
