// Host-side unit tests for the pairing-PIN channel (pairing.h).
//
// `pio test -e native` — no hardware, no radio, no panel. The reason this has
// tests at all is that it is the one place a SECRET crosses a module boundary,
// and every property that makes that acceptable is testable without hardware:
//
//   * the policy (shouldShow) is exactly "AP up AND nothing paired", including
//     that it comes BACK when the last session goes away;
//   * nothing is stored unless a consumer subscribed, so with the LCD off the
//     PIN exists in mod_http's buffer and nowhere else;
//   * unsubscribing and withdraw() actually wipe the buffer;
//   * get() is bounded, so a caller with a short buffer truncates instead of
//     overrunning.

#include <unity.h>

#include <string.h>

#include "pairing.h"

void setUp() { Pairing::subscribe(false); }
void tearDown() { Pairing::subscribe(false); }

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
  char out[16];
  Pairing::publish("12345678");
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_EQUAL_size_t(0, Pairing::get(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_unsubscribe_wipes_a_published_pin() {
  char out[16];
  Pairing::subscribe(true);
  Pairing::publish("12345678");
  TEST_ASSERT_TRUE(Pairing::visible());

  Pairing::subscribe(false);
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_EQUAL_size_t(0, Pairing::get(out, sizeof(out)));
}

// ---- publish / withdraw -------------------------------------------------

void test_publish_and_get() {
  char out[16];
  Pairing::subscribe(true);
  Pairing::publish("40718293");
  TEST_ASSERT_TRUE(Pairing::visible());
  TEST_ASSERT_EQUAL_size_t(8, Pairing::get(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("40718293", out);
}

void test_withdraw_clears() {
  char out[16];
  Pairing::subscribe(true);
  Pairing::publish("40718293");
  Pairing::withdraw();
  TEST_ASSERT_FALSE(Pairing::visible());
  TEST_ASSERT_EQUAL_size_t(0, Pairing::get(out, sizeof(out)));
}

void test_publish_null_or_empty_withdraws() {
  Pairing::subscribe(true);
  Pairing::publish("40718293");
  Pairing::publish(nullptr);
  TEST_ASSERT_FALSE(Pairing::visible());

  Pairing::publish("40718293");
  Pairing::publish("");
  TEST_ASSERT_FALSE(Pairing::visible());
}

// ---- seq(), which is what drives the repaint ----------------------------

void test_republishing_the_same_pin_does_not_move_seq() {
  // mod_http calls publish() from a 250 ms tick. If an unchanged value moved
  // the sequence counter the LCD would repaint the PIN four times a second
  // forever.
  Pairing::subscribe(true);
  Pairing::publish("40718293");
  uint32_t after = Pairing::seq();
  for (int i = 0; i < 20; i++) {
    Pairing::publish("40718293");
  }
  TEST_ASSERT_EQUAL_UINT32(after, Pairing::seq());
}

void test_seq_moves_on_a_new_pin_and_on_withdraw() {
  Pairing::subscribe(true);
  Pairing::publish("40718293");
  uint32_t a = Pairing::seq();
  Pairing::publish("11112222");  // `pin regen` replaced it
  uint32_t b = Pairing::seq();
  TEST_ASSERT_NOT_EQUAL(a, b);
  Pairing::withdraw();
  TEST_ASSERT_NOT_EQUAL(b, Pairing::seq());
}

void test_withdraw_when_already_clear_does_not_move_seq() {
  Pairing::subscribe(true);
  uint32_t a = Pairing::seq();
  Pairing::withdraw();
  TEST_ASSERT_EQUAL_UINT32(a, Pairing::seq());
}

// ---- bounds -------------------------------------------------------------

void test_get_truncates_into_a_short_buffer() {
  char out[4];
  Pairing::subscribe(true);
  Pairing::publish("12345678");
  TEST_ASSERT_EQUAL_size_t(3, Pairing::get(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("123", out);
}

void test_get_rejects_degenerate_buffers() {
  char out[4] = {'x', 'x', 'x', 0};
  Pairing::subscribe(true);
  Pairing::publish("12345678");
  TEST_ASSERT_EQUAL_size_t(0, Pairing::get(nullptr, 4));
  TEST_ASSERT_EQUAL_size_t(0, Pairing::get(out, 0));
  TEST_ASSERT_EQUAL_STRING("xxx", out);  // untouched
}

void test_publish_bounds_an_overlong_pin() {
  char out[64];
  Pairing::subscribe(true);
  Pairing::publish("0123456789abcdef0123456789abcdef");  // 32 chars, twice MAX_PIN
  TEST_ASSERT_EQUAL_size_t(Pairing::MAX_PIN, Pairing::get(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef", out);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_policy_shows_only_while_unpaired_and_up);
  RUN_TEST(test_policy_returns_when_the_last_session_goes_away);
  RUN_TEST(test_publish_stores_nothing_while_unsubscribed);
  RUN_TEST(test_unsubscribe_wipes_a_published_pin);
  RUN_TEST(test_publish_and_get);
  RUN_TEST(test_withdraw_clears);
  RUN_TEST(test_publish_null_or_empty_withdraws);
  RUN_TEST(test_republishing_the_same_pin_does_not_move_seq);
  RUN_TEST(test_seq_moves_on_a_new_pin_and_on_withdraw);
  RUN_TEST(test_withdraw_when_already_clear_does_not_move_seq);
  RUN_TEST(test_get_truncates_into_a_short_buffer);
  RUN_TEST(test_get_rejects_degenerate_buffers);
  RUN_TEST(test_publish_bounds_an_overlong_pin);
  return UNITY_END();
}
