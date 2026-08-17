// Host-side unit tests for RateLimit (ratelimit.h).
//
// `pio test -e native` — no hardware, and a synthetic clock. That is the point:
// the states worth testing are 15 minutes apart (the lockout) and 49.7 days
// apart (the millis() wrap), and neither is reachable in a hardware test that
// anyone would actually run.

#include <unity.h>

#include "ratelimit.h"

using namespace RateLimit;

void setUp() {}
void tearDown() {}

// Drives `n` failures back to back, honouring each penalty so that check()
// always returns ALLOW before fail() is called — the documented contract.
static uint32_t failTimes(State &s, uint32_t now, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    uint32_t retry = 0;
    Decision d = check(s, now, &retry);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ALLOW, d, "test helper stepped the clock wrong");
    fail(s, now);
    now += penaltyMs(s.fails) + 1;
  }
  return now;
}

void test_first_attempt_is_allowed() {
  State s;
  uint32_t retry = 12345;
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, 0, &retry));
  TEST_ASSERT_EQUAL_UINT32(0, retry);
  TEST_ASSERT_EQUAL_UINT8(MAX_FAILS, remaining(s));
}

void test_penalty_doubles_then_caps() {
  TEST_ASSERT_EQUAL_UINT32(0, penaltyMs(0));
  TEST_ASSERT_EQUAL_UINT32(1000, penaltyMs(1));
  TEST_ASSERT_EQUAL_UINT32(2000, penaltyMs(2));
  TEST_ASSERT_EQUAL_UINT32(4000, penaltyMs(3));
  TEST_ASSERT_EQUAL_UINT32(8000, penaltyMs(4));
  TEST_ASSERT_EQUAL_UINT32(16000, penaltyMs(5));
  TEST_ASSERT_EQUAL_UINT32(MAX_DELAY_MS, penaltyMs(6));
  TEST_ASSERT_EQUAL_UINT32(MAX_DELAY_MS, penaltyMs(9));
  TEST_ASSERT_EQUAL_UINT32(MAX_DELAY_MS, penaltyMs(200));
}

void test_a_failure_blocks_the_next_attempt_until_the_penalty_expires() {
  State s;
  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, 1000, &retry));
  fail(s, 1000);

  TEST_ASSERT_EQUAL_UINT8(WAIT, check(s, 1000, &retry));
  TEST_ASSERT_EQUAL_UINT32(1000, retry);
  TEST_ASSERT_EQUAL_UINT8(WAIT, check(s, 1999, &retry));
  TEST_ASSERT_EQUAL_UINT32(1, retry);
  // Exactly at the deadline is allowed: >=, not >.
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, 2000, &retry));
  TEST_ASSERT_EQUAL_UINT32(0, retry);
}

void test_remaining_counts_down_and_hits_zero_at_the_lockout() {
  State s;
  uint32_t now = failTimes(s, 5000, (uint8_t)(MAX_FAILS - 1));
  TEST_ASSERT_EQUAL_UINT8(1, remaining(s));
  TEST_ASSERT_FALSE(s.locked);

  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, now, &retry));
  fail(s, now);
  TEST_ASSERT_TRUE(s.locked);
  TEST_ASSERT_EQUAL_UINT8(0, remaining(s));
  TEST_ASSERT_EQUAL_UINT32(1, s.lockouts);
  TEST_ASSERT_EQUAL_UINT32(MAX_FAILS, s.totalFails);
}

void test_lockout_reports_locked_until_it_expires_then_starts_clean() {
  State s;
  // Inlined rather than using failTimes(), because this test needs the instant
  // the lockout STARTED, not the next-allowed time the helper returns.
  uint32_t lockedAt = 0;
  for (uint8_t i = 0; i < MAX_FAILS; i++) {
    uint32_t r = 0;
    TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, lockedAt, &r));
    fail(s, lockedAt);
    if (i + 1 < MAX_FAILS) {
      lockedAt += penaltyMs(s.fails) + 1;
    }
  }

  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(LOCKED, check(s, lockedAt, &retry));
  TEST_ASSERT_EQUAL_UINT32(LOCKOUT_MS, retry);
  TEST_ASSERT_EQUAL_UINT8(LOCKED, check(s, lockedAt + LOCKOUT_MS - 1, &retry));
  TEST_ASSERT_EQUAL_UINT32(1, retry);

  // Served. The escalation restarts from zero rather than resuming one guess
  // below the lockout, which would make a second lockout arrive immediately.
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, lockedAt + LOCKOUT_MS, &retry));
  TEST_ASSERT_EQUAL_UINT8(MAX_FAILS, remaining(s));
  TEST_ASSERT_FALSE(s.locked);
  // Lifetime counters SURVIVE the reset: status has to be able to say the
  // device has been ground on, even after the lockout lapsed.
  TEST_ASSERT_EQUAL_UINT32(MAX_FAILS, s.totalFails);
  TEST_ASSERT_EQUAL_UINT32(1, s.lockouts);
}

void test_success_clears_the_escalation_but_not_the_history() {
  State s;
  uint32_t now = failTimes(s, 100, 3);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(MAX_FAILS - 3), remaining(s));
  success(s);
  uint32_t retry = 123;
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, now, &retry));
  TEST_ASSERT_EQUAL_UINT32(0, retry);
  TEST_ASSERT_EQUAL_UINT8(MAX_FAILS, remaining(s));
  TEST_ASSERT_EQUAL_UINT32(3, s.totalFails);
}

void test_success_also_clears_a_lockout() {
  State s;
  failTimes(s, 0, MAX_FAILS);
  TEST_ASSERT_TRUE(s.locked);
  success(s);
  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, 1, &retry));
}

// millis() wraps to 0 after 49.7 days. A `now >= deadline` comparison would
// treat a deadline set just before the wrap as unreachable and lock the
// endpoint out for the next 49.7 days; the signed-difference form does not.
void test_penalty_survives_the_millis_wrap() {
  State s;
  uint32_t now = 0xFFFFFF00u;  // 256 ms before the wrap
  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, now, &retry));
  // deadline = 0xFFFFFF00 + 1000, i.e. 744 (0x2E8) ms PAST the wrap.
  fail(s, now);

  TEST_ASSERT_EQUAL_UINT8(WAIT, check(s, now, &retry));
  TEST_ASSERT_EQUAL_UINT32(1000, retry);
  TEST_ASSERT_EQUAL_UINT8(WAIT, check(s, 0x00000100u, &retry));  // after the wrap, still early
  TEST_ASSERT_EQUAL_UINT32(0x2E8u - 0x100u, retry);
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, 0x000002E8u, &retry));
}

void test_lockout_survives_the_millis_wrap() {
  State s;
  uint32_t now = 0xFFFF0000u;
  for (uint8_t i = 0; i < MAX_FAILS; i++) {
    uint32_t retry = 0;
    TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, now, &retry));
    fail(s, now);
    now += penaltyMs(s.fails) + 1;
  }
  TEST_ASSERT_TRUE(s.locked);
  uint32_t unlockAt = s.lockUntilMs;
  uint32_t retry = 0;
  TEST_ASSERT_EQUAL_UINT8(LOCKED, check(s, unlockAt - 1, &retry));
  TEST_ASSERT_EQUAL_UINT32(1, retry);
  TEST_ASSERT_EQUAL_UINT8(ALLOW, check(s, unlockAt, &retry));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_attempt_is_allowed);
  RUN_TEST(test_penalty_doubles_then_caps);
  RUN_TEST(test_a_failure_blocks_the_next_attempt_until_the_penalty_expires);
  RUN_TEST(test_remaining_counts_down_and_hits_zero_at_the_lockout);
  RUN_TEST(test_lockout_reports_locked_until_it_expires_then_starts_clean);
  RUN_TEST(test_success_clears_the_escalation_but_not_the_history);
  RUN_TEST(test_success_also_clears_a_lockout);
  RUN_TEST(test_penalty_survives_the_millis_wrap);
  RUN_TEST(test_lockout_survives_the_millis_wrap);
  return UNITY_END();
}
