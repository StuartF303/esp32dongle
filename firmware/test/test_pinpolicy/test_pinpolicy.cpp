// Host-side unit tests for PinPolicy (pinpolicy.h).
//
// `pio test -e native`. This suite exists because the decision it covers — what
// a PIN attempt does to the rate limiter, and whether it mints a new PIN — is
// the invariant the 4-digit PIN rests on, and it had no coverage on either
// side: test_ratelimit asserts the limiter's own state machine, test_ct asserts
// the comparison, and the code BETWEEN them was reachable only over the air.
// handleSessionCreate() is registered on the softAP listener alone and this
// machine has no 802.11 PHY, so it cannot be exercised from here at all. These
// assertions are the only ones that will ever exist for it on this machine.
//
// THE REGRESSION THIS IS REALLY FOR is test_the_tenth_failure_mints_but_the
// _lockout_still_stands. If a future edit "tidies" the mint path by routing it
// through the same helper the operator's regenerate uses, ten wrong guesses
// would clear the limiter, the 15-minute lockout would never be served, and
// nothing else in the tree would notice.

#include <unity.h>

#include "pinpolicy.h"

using namespace PinPolicy;

void setUp() {}
void tearDown() {}

// Drives `n` failures back to back, honouring each penalty so that check()
// returns ALLOW before every attempt — afterAttempt()'s documented
// precondition. Returns the clock afterwards.
static uint32_t failTimes(RateLimit::State &s, uint32_t now, uint8_t n, Outcome *last) {
  for (uint8_t i = 0; i < n; i++) {
    uint32_t retry = 0;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(RateLimit::ALLOW, RateLimit::check(s, now, &retry),
                                    "test helper stepped the clock wrong");
    Outcome o = afterAttempt(s, false, now);
    if (last != nullptr) {
      *last = o;
    }
    now += RateLimit::penaltyMs(s.fails) + 1;
  }
  return now;
}

// ---- the ordinary cases --------------------------------------------------

void test_a_correct_pin_mints_and_clears() {
  RateLimit::State s;
  // Some failures first, so "clears" means something.
  failTimes(s, 1000, 3, nullptr);
  TEST_ASSERT_EQUAL_UINT8(3, s.fails);

  Outcome o = afterAttempt(s, true, 100000);
  // Minted because the PIN is SINGLE-USE — pairing spends it. Not because
  // anything went wrong.
  TEST_ASSERT_TRUE(o.mint);
  TEST_ASSERT_TRUE(o.clearLimiter);
  TEST_ASSERT_EQUAL_UINT8(0, s.fails);
  TEST_ASSERT_FALSE(s.locked);
  TEST_ASSERT_FALSE(s.hasDeadline);
  TEST_ASSERT_EQUAL_UINT8(RateLimit::MAX_FAILS, RateLimit::remaining(s));
}

void test_the_first_nine_failures_do_not_mint() {
  RateLimit::State s;
  uint32_t now = 1000;
  for (uint8_t i = 1; i < RateLimit::MAX_FAILS; i++) {
    uint32_t retry = 0;
    TEST_ASSERT_EQUAL_UINT8(RateLimit::ALLOW, RateLimit::check(s, now, &retry));
    Outcome o = afterAttempt(s, false, now);
    // Rotating on every wrong guess would hand any passer-by a way to change
    // the digits on the LCD from behind a locked screen, and would make the
    // PIN unreadable for anyone legitimately typing it.
    TEST_ASSERT_FALSE_MESSAGE(o.mint, "an ordinary wrong guess must not rotate the PIN");
    TEST_ASSERT_FALSE(o.clearLimiter);
    TEST_ASSERT_EQUAL_UINT8(i, s.fails);
    TEST_ASSERT_FALSE(s.locked);
    now += RateLimit::penaltyMs(s.fails) + 1;
  }
  TEST_ASSERT_EQUAL_UINT8(1, RateLimit::remaining(s));
}

// ---- THE ONE THAT MATTERS ------------------------------------------------

void test_the_tenth_failure_mints_but_the_lockout_still_stands() {
  RateLimit::State s;
  Outcome last{false, false};
  uint32_t now = failTimes(s, 1000, RateLimit::MAX_FAILS, &last);

  // Minted: this is what makes 10^4 defensible, because the next 10 guesses
  // face a fresh space rather than a space with 10 values eliminated.
  TEST_ASSERT_TRUE_MESSAGE(last.mint, "the lockout must mint a new PIN");
  // NOT cleared: the 15 minutes are still owed. If this ever flips, ten guesses
  // buy a free limiter reset and the limiter stops being a control at all.
  TEST_ASSERT_FALSE_MESSAGE(last.clearLimiter, "the lockout must NOT clear the rate limiter");

  TEST_ASSERT_TRUE(s.locked);
  TEST_ASSERT_EQUAL_UINT8(RateLimit::MAX_FAILS, s.fails);
  TEST_ASSERT_EQUAL_UINT8(0, RateLimit::remaining(s));
  TEST_ASSERT_EQUAL_UINT32(1, s.lockouts);

  // And the lockout is really served — the whole 15 minutes, from the limiter's
  // own point of view, with the mint having changed nothing about it.
  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(RateLimit::LOCKED, RateLimit::check(s, now, &retry));
  uint32_t unlockAt = s.lockUntilMs;
  TEST_ASSERT_EQUAL_UINT8(RateLimit::LOCKED, RateLimit::check(s, unlockAt - (5u * 60u * 1000u), &retry));
  TEST_ASSERT_EQUAL_UINT8(RateLimit::LOCKED, RateLimit::check(s, unlockAt - 1, &retry));
  TEST_ASSERT_EQUAL_UINT32(1, retry);
  // Only then.
  TEST_ASSERT_EQUAL_UINT8(RateLimit::ALLOW, RateLimit::check(s, unlockAt, &retry));
}

// A second lockout must behave exactly like the first: the counter restarts
// clean after the wait (ratelimit.h's choice), and the tenth failure of the new
// run mints again.
void test_a_second_lockout_mints_again() {
  RateLimit::State s;
  Outcome last{false, false};
  failTimes(s, 1000, RateLimit::MAX_FAILS, &last);
  TEST_ASSERT_TRUE(last.mint);

  uint32_t retry = 0;
  uint32_t after = s.lockUntilMs;
  TEST_ASSERT_EQUAL_UINT8(RateLimit::ALLOW, RateLimit::check(s, after, &retry));
  TEST_ASSERT_EQUAL_UINT8(0, s.fails);

  last = Outcome{false, false};
  failTimes(s, after, RateLimit::MAX_FAILS, &last);
  TEST_ASSERT_TRUE(last.mint);
  TEST_ASSERT_FALSE(last.clearLimiter);
  TEST_ASSERT_TRUE(s.locked);
  TEST_ASSERT_EQUAL_UINT32(2, s.lockouts);
}

// ---- the operator's route ------------------------------------------------

void test_operator_regenerate_mints_and_clears_a_lockout() {
  RateLimit::State s;
  failTimes(s, 1000, RateLimit::MAX_FAILS, nullptr);
  TEST_ASSERT_TRUE(s.locked);

  Outcome o = operatorRegenerate(s);
  // AUTH_PHYSICAL: the caller is holding the USB cable, so they are the owner
  // and not the party the limiter defends against. A limiter the owner cannot
  // clear is a denial of service with no recovery.
  TEST_ASSERT_TRUE(o.mint);
  TEST_ASSERT_TRUE(o.clearLimiter);
  TEST_ASSERT_FALSE(s.locked);
  TEST_ASSERT_EQUAL_UINT8(0, s.fails);
  TEST_ASSERT_EQUAL_UINT8(RateLimit::MAX_FAILS, RateLimit::remaining(s));

  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(RateLimit::ALLOW, RateLimit::check(s, 2000, &retry));
}

// The lifetime counters are telemetry and must survive both routes — status()
// reports them, and a limiter reset that also erased the history would hide a
// grinding attempt from whoever is looking at the device.
void test_neither_route_erases_the_lifetime_counters() {
  RateLimit::State s;
  failTimes(s, 1000, RateLimit::MAX_FAILS, nullptr);
  TEST_ASSERT_EQUAL_UINT32(RateLimit::MAX_FAILS, s.totalFails);
  TEST_ASSERT_EQUAL_UINT32(1, s.lockouts);

  operatorRegenerate(s);
  TEST_ASSERT_EQUAL_UINT32(RateLimit::MAX_FAILS, s.totalFails);
  TEST_ASSERT_EQUAL_UINT32(1, s.lockouts);

  afterAttempt(s, true, 99999);
  TEST_ASSERT_EQUAL_UINT32(RateLimit::MAX_FAILS, s.totalFails);
  TEST_ASSERT_EQUAL_UINT32(1, s.lockouts);
}

// ---- the wrap ------------------------------------------------------------

// The lockout minted across the millis() wrap must still be served in full.
// Nothing in this file does arithmetic on the clock, but it hands `nowMs`
// straight to the limiter, so the composition is worth pinning.
void test_the_lockout_it_mints_survives_the_millis_wrap() {
  RateLimit::State s;
  Outcome last{false, false};
  failTimes(s, 0xFFFF0000u, RateLimit::MAX_FAILS, &last);
  TEST_ASSERT_TRUE(last.mint);
  TEST_ASSERT_FALSE(last.clearLimiter);
  TEST_ASSERT_TRUE(s.locked);

  uint32_t retry = 0;
  uint32_t unlockAt = s.lockUntilMs;  // past the wrap
  TEST_ASSERT_EQUAL_UINT8(RateLimit::LOCKED, RateLimit::check(s, unlockAt - 1, &retry));
  TEST_ASSERT_EQUAL_UINT32(1, retry);
  TEST_ASSERT_EQUAL_UINT8(RateLimit::ALLOW, RateLimit::check(s, unlockAt, &retry));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_a_correct_pin_mints_and_clears);
  RUN_TEST(test_the_first_nine_failures_do_not_mint);
  RUN_TEST(test_the_tenth_failure_mints_but_the_lockout_still_stands);
  RUN_TEST(test_a_second_lockout_mints_again);
  RUN_TEST(test_operator_regenerate_mints_and_clears_a_lockout);
  RUN_TEST(test_neither_route_erases_the_lifetime_counters);
  RUN_TEST(test_the_lockout_it_mints_survives_the_millis_wrap);
  return UNITY_END();
}
