// Host-side unit tests for ApGrace (apgrace.h).
//
// `pio test -e native` — no hardware, no radio, a synthetic clock, a synthetic
// station count and a synthetic "is a session live". That is the whole reason
// the state machine is a header instead of inline logic in mod_http.cpp's tick:
// the cases that matter are a 90-second window and the 49.7-day millis() wrap,
// and neither is something anyone would ever sit through on the device.
//
// What these assert, in the order they matter:
//   * reassociation inside the window CANCELS the expiry (the common case — a
//     phone bouncing off the AP must not cost the user their session);
//   * NO SESSION MEANS NO CLOCK, which is a decision (stuart, 2026-08-24) and
//     not an emergent property: a station that associates to read the PIN and
//     leaves without pairing must not rotate the digits from under whoever is
//     still typing them;
//   * expiry fires EXACTLY ONCE (the tick runs every 250 ms; a repeating
//     EXPIRED would re-mint the PIN four times a second forever);
//   * the wrap at 2^32 does not swallow or fabricate an expiry;
//   * reset() really does forget everything, so a disabled module cannot arm a
//     clock against an AP that is not there.

#include <unity.h>

#include "apgrace.h"

using namespace ApGrace;

void setUp() {}
void tearDown() {}

// Steps the clock in 250 ms ticks (mod_http.cpp's TICK_MS) from `from` to `to`
// with the given station count and session state, and returns how many EXPIRED
// events came out. Ticking rather than jumping is the point: the real caller
// polls.
static uint8_t tickThrough(State &s, uint8_t stations, bool sessionLive, uint32_t from, uint32_t to) {
  uint8_t fired = 0;
  for (uint32_t t = from; (int32_t)(t - to) <= 0; t += 250) {
    if (update(s, stations, sessionLive, t) == EXPIRED) {
      fired++;
    }
  }
  return fired;
}

// ---- no session, no clock ------------------------------------------------

void test_nothing_happens_while_nobody_is_paired() {
  State s;
  // Fresh boot, AP up, nobody connected and nothing paired. Ten minutes of
  // ticks must not arm anything: there is no session to end.
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 0, false, 0, 600000));
  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT32(0, remainingMs(s, 600000));
}

// THE ONE THIS RULE EXISTS FOR. Someone joins the AP to read the PIN off the
// LCD (or to scan the pair QR, which carries it), their phone drops the network
// before they finish, and they are still typing. Ninety seconds later the
// digits must NOT have changed under them.
void test_a_station_that_associates_and_leaves_without_pairing_arms_nothing() {
  State s;
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 1, false, 1000));  // associated, unpaired
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, false, 2000));  // gone again
  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 0, false, 2000, 2000 + 4 * GRACE_MS));
}

// The session going away by any other route (explicit unpair, `sessions
// revoke`, the idle timeout) must take the clock with it — the revoke has
// already dealt with the session, and a surviving deadline would rotate a PIN
// for a session that no longer exists.
void test_losing_the_session_cancels_a_running_clock() {
  State s;
  update(s, 1, true, 0);
  update(s, 0, true, 1000);  // phone drops off, clock arms
  TEST_ASSERT_TRUE(armed(s));

  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, false, 1000 + GRACE_MS / 2));
  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 0, false, 1000 + GRACE_MS / 2, 1000 + 3 * GRACE_MS));
}

void test_an_associated_station_never_arms_the_clock() {
  State s;
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 1, true, 1000));
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 1, true, 1000, 1000 + 5 * GRACE_MS));
  TEST_ASSERT_FALSE(armed(s));
}

// ---- arming and the countdown -------------------------------------------

void test_losing_the_station_arms_the_clock_and_reports_the_time_left() {
  State s;
  update(s, 1, true, 1000);
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, 2000));
  TEST_ASSERT_TRUE(armed(s));
  TEST_ASSERT_EQUAL_UINT32(GRACE_MS, remainingMs(s, 2000));
  TEST_ASSERT_EQUAL_UINT32(GRACE_MS - 1000, remainingMs(s, 3000));
  TEST_ASSERT_EQUAL_UINT32(1, remainingMs(s, 2000 + GRACE_MS - 1));
  // At and past the deadline the countdown reads zero rather than wrapping to
  // 4.29 billion, which is what an unsigned subtraction would give the UI.
  TEST_ASSERT_EQUAL_UINT32(0, remainingMs(s, 2000 + GRACE_MS));
  TEST_ASSERT_EQUAL_UINT32(0, remainingMs(s, 2000 + GRACE_MS + 5000));
}

void test_the_deadline_is_inclusive() {
  State s;
  update(s, 1, true, 0);
  update(s, 0, true, 0);
  // >=, not >: one tick landing exactly on the deadline must end the session,
  // the same rule ratelimit.h uses for its penalties.
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, GRACE_MS - 1));
  TEST_ASSERT_EQUAL_UINT8(EXPIRED, update(s, 0, true, GRACE_MS));
}

// A session that exists with no station arms the clock even if this file never
// saw the station — the session could only have been created by an associated
// client, so its absence now IS a departure. Documented at update().
void test_a_live_session_with_no_station_arms_without_a_seen_departure() {
  State s;
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, 5000));
  TEST_ASSERT_TRUE(armed(s));
  TEST_ASSERT_EQUAL_UINT32(GRACE_MS, remainingMs(s, 5000));
  TEST_ASSERT_EQUAL_UINT8(EXPIRED, update(s, 0, true, 5000 + GRACE_MS));
}

// ---- the case this whole file exists for ---------------------------------

void test_reassociation_inside_the_window_cancels_the_expiry() {
  State s;
  update(s, 1, true, 10000);
  update(s, 0, true, 20000);  // phone drops off
  TEST_ASSERT_TRUE(armed(s));

  // Back with a second to spare. The token in localStorage is still valid and
  // the session must survive untouched.
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 1, true, 20000 + GRACE_MS - 1000));
  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT32(0, remainingMs(s, 20000 + GRACE_MS - 1000));

  // And the cancelled deadline must not come back to life afterwards.
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 1, true, 20000 + GRACE_MS, 20000 + 4 * GRACE_MS));
}

void test_a_second_departure_gets_a_full_window() {
  State s;
  update(s, 1, true, 0);
  update(s, 0, true, 1000);                 // away
  update(s, 1, true, 1000 + GRACE_MS / 2);  // back, half way through
  uint32_t away = 1000 + GRACE_MS;
  update(s, 0, true, away);  // away again
  // A fresh 90 s, not the remainder of the first window.
  TEST_ASSERT_EQUAL_UINT32(GRACE_MS, remainingMs(s, away));
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, away + GRACE_MS - 1));
  TEST_ASSERT_EQUAL_UINT8(EXPIRED, update(s, 0, true, away + GRACE_MS));
}

void test_expiry_fires_exactly_once_however_long_the_ticks_run() {
  State s;
  update(s, 1, true, 5000);
  // An hour of 250 ms ticks with nobody there, and sessionLive left TRUE
  // throughout — i.e. a caller that ignores the event entirely. EXPIRED once.
  //
  // This is the latch's test. Written the obvious way (arm whenever a live
  // session has no station) this returned 39: the inputs never change, so it
  // re-armed on the next tick and expired again every 90 seconds, rotating the
  // PIN each time. The real tick revokes the session on EXPIRED and would have
  // hidden it. The guarantee belongs to the component, so it is asserted
  // without the caller's help.
  TEST_ASSERT_EQUAL_UINT8(1, tickThrough(s, 0, true, 5000, 5000 + 3600000));
  TEST_ASSERT_FALSE(armed(s));
}

void test_a_station_returning_after_expiry_starts_a_whole_new_cycle() {
  State s;
  update(s, 1, true, 0);
  update(s, 0, true, 0);
  TEST_ASSERT_EQUAL_UINT8(EXPIRED, update(s, 0, true, GRACE_MS));
  // The session was revoked, so the next ticks carry sessionLive=false...
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 0, false, GRACE_MS, GRACE_MS + 60000));
  // ...and someone pairs again later. The machine must be in its initial shape,
  // not holding a dead deadline.
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 1, true, GRACE_MS + 60000));
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, GRACE_MS + 61000));
  TEST_ASSERT_TRUE(armed(s));
  TEST_ASSERT_EQUAL_UINT8(EXPIRED, update(s, 0, true, GRACE_MS + 61000 + GRACE_MS));
}

// ---- the wrap ------------------------------------------------------------

void test_the_window_survives_the_millis_wrap() {
  State s;
  // Arm 1000 ms before millis() wraps, so the deadline is GRACE_MS - 1000 ms
  // PAST zero. `now >= deadline` would be false forever here and the session
  // would be held for the next 49.7 days.
  const uint32_t now = 0xFFFFFFFFu - 999u;  // wraps in 1000 ms
  update(s, 1, true, now);
  update(s, 0, true, now);
  const uint32_t deadline = GRACE_MS - 1000u;  // after the wrap

  TEST_ASSERT_EQUAL_UINT32(GRACE_MS, remainingMs(s, now));
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, 0));  // the instant of the wrap
  TEST_ASSERT_EQUAL_UINT32(deadline, remainingMs(s, 0));
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, true, deadline - 1));
  TEST_ASSERT_EQUAL_UINT8(EXPIRED, update(s, 0, true, deadline));
}

void test_reassociation_across_the_wrap_still_cancels() {
  State s;
  const uint32_t now = 0xFFFFFFFFu - 499u;
  update(s, 1, true, now);
  update(s, 0, true, now);
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 1, true, 250));  // after the wrap, in time
  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 1, true, 250, 250 + 2 * GRACE_MS));
}

// ---- teardown ------------------------------------------------------------

void test_reset_forgets_everything() {
  State s;
  update(s, 1, true, 1000);
  update(s, 0, true, 2000);
  TEST_ASSERT_TRUE(armed(s));

  reset(s);  // `disable http`, or the AP going down under us

  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT32(0, remainingMs(s, 2000));
  // The deadline that was pending must not fire after the reset. It would fire
  // against a session belonging to an AP that is no longer on the air.
  TEST_ASSERT_EQUAL_UINT8(0, tickThrough(s, 0, false, 2000, 2000 + 2 * GRACE_MS));
  TEST_ASSERT_FALSE(armed(s));
}

void test_reset_is_idempotent_and_safe_on_a_fresh_state() {
  State s;
  reset(s);
  reset(s);
  TEST_ASSERT_FALSE(armed(s));
  TEST_ASSERT_EQUAL_UINT8(NONE, update(s, 0, false, 12345));
  TEST_ASSERT_FALSE(armed(s));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_happens_while_nobody_is_paired);
  RUN_TEST(test_a_station_that_associates_and_leaves_without_pairing_arms_nothing);
  RUN_TEST(test_losing_the_session_cancels_a_running_clock);
  RUN_TEST(test_an_associated_station_never_arms_the_clock);
  RUN_TEST(test_losing_the_station_arms_the_clock_and_reports_the_time_left);
  RUN_TEST(test_the_deadline_is_inclusive);
  RUN_TEST(test_a_live_session_with_no_station_arms_without_a_seen_departure);
  RUN_TEST(test_reassociation_inside_the_window_cancels_the_expiry);
  RUN_TEST(test_a_second_departure_gets_a_full_window);
  RUN_TEST(test_expiry_fires_exactly_once_however_long_the_ticks_run);
  RUN_TEST(test_a_station_returning_after_expiry_starts_a_whole_new_cycle);
  RUN_TEST(test_the_window_survives_the_millis_wrap);
  RUN_TEST(test_reassociation_across_the_wrap_still_cancels);
  RUN_TEST(test_reset_forgets_everything);
  RUN_TEST(test_reset_is_idempotent_and_safe_on_a_fresh_state);
  return UNITY_END();
}
