// Host-side unit tests for the OTA rollback-confirmation state machine
// (otadecide.h).
//
// `pio test -e native` — no hardware, no flash, no partitions. This is the
// whole of backlog S4's decision logic: which health criteria are met given a
// set of inputs, and what the (pending? x criteria x elapsed) table decides.
//
// Worth testing here rather than on the device because the failure mode is
// silent AND DELAYED. A criteria mask assembled with the wrong operator, or a
// window compared with > instead of >=, produces a device that works perfectly
// today and quietly discards the next OTA at the following power cycle — with
// nothing in any log tying the two together. There is no bench observation that
// catches that; only the table does.
//
// The Config under test is OtaDecide::DEFAULTS, i.e. the shipped numbers, not a
// copy of them.

#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "otadecide.h"

void setUp() {}
void tearDown() {}

namespace {

// A device that is doing everything right, well past every threshold.
OtaDecide::Inputs healthy() {
  OtaDecide::Inputs in;
  in.uptimeMs = OtaDecide::DEFAULTS.minUptimeMs;
  in.ticks = OtaDecide::DEFAULTS.minTicks;
  in.registryFatal = false;
  in.essentialEnabled = true;
  in.consoleAnswered = true;
  return in;
}

const OtaDecide::Config &CFG = OtaDecide::DEFAULTS;

}  // namespace

// ---- the criteria bits ---------------------------------------------------

void test_crit_all_is_exactly_the_five_bits() {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < OtaDecide::CRIT_COUNT; i++) {
    // Each bit distinct: an accidental duplicate in CRIT_BITS would make
    // CRIT_ALL reachable with one criterion never checked.
    TEST_ASSERT_EQUAL_UINT8(0, sum & OtaDecide::CRIT_BITS[i]);
    sum |= OtaDecide::CRIT_BITS[i];
  }
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL, sum);
}

void test_every_bit_has_a_stable_name() {
  for (uint8_t i = 0; i < OtaDecide::CRIT_COUNT; i++) {
    const char *n = OtaDecide::critName(OtaDecide::CRIT_BITS[i]);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_TRUE(strcmp(n, "?") != 0);
  }
  // The wire names. These appear in the `ota` command's `criteria` object and
  // in the ota.* bus events, so renaming one is a protocol change.
  TEST_ASSERT_EQUAL_STRING("ticks", OtaDecide::critName(OtaDecide::CRIT_TICKS));
  TEST_ASSERT_EQUAL_STRING("registry", OtaDecide::critName(OtaDecide::CRIT_REGISTRY));
  TEST_ASSERT_EQUAL_STRING("essential", OtaDecide::critName(OtaDecide::CRIT_ESSENTIAL));
  TEST_ASSERT_EQUAL_STRING("console", OtaDecide::critName(OtaDecide::CRIT_CONSOLE));
  TEST_ASSERT_EQUAL_STRING("uptime", OtaDecide::critName(OtaDecide::CRIT_UPTIME));
  TEST_ASSERT_EQUAL_STRING("?", OtaDecide::critName(0x80));
}

void test_healthy_device_meets_everything() {
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL, OtaDecide::criteriaMet(healthy(), CFG));
}

void test_each_criterion_can_fail_on_its_own() {
  // One at a time, so a bit that is wired to the wrong input shows up as the
  // WRONG bit going missing rather than as a vague "not all met".
  OtaDecide::Inputs in = healthy();
  in.ticks = CFG.minTicks - 1;
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL & ~OtaDecide::CRIT_TICKS, OtaDecide::criteriaMet(in, CFG));

  in = healthy();
  in.registryFatal = true;
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL & ~OtaDecide::CRIT_REGISTRY, OtaDecide::criteriaMet(in, CFG));

  in = healthy();
  in.essentialEnabled = false;
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL & ~OtaDecide::CRIT_ESSENTIAL, OtaDecide::criteriaMet(in, CFG));

  // CRIT_CONSOLE needs BOTH halves of its OR to be false: no answered request
  // AND the grace period still running.
  in = healthy();
  in.consoleAnswered = false;
  in.uptimeMs = CFG.consoleGraceMs - 1;
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL & ~(OtaDecide::CRIT_CONSOLE | OtaDecide::CRIT_UPTIME),
                          OtaDecide::criteriaMet(in, CFG));

  in = healthy();
  in.uptimeMs = CFG.minUptimeMs - 1;
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL & ~OtaDecide::CRIT_UPTIME, OtaDecide::criteriaMet(in, CFG));
}

void test_console_criterion_is_an_or_not_an_and() {
  // THE point of CRIT_CONSOLE's shape. A device that was OTA'd and left on a
  // desk with no cable and no phone must still confirm itself; requiring a
  // console line would roll back every unattended update.
  OtaDecide::Inputs in = healthy();
  in.consoleAnswered = false;
  in.uptimeMs = CFG.consoleGraceMs;
  TEST_ASSERT_TRUE((OtaDecide::criteriaMet(in, CFG) & OtaDecide::CRIT_CONSOLE) != 0);

  // And the other half: someone talking to it early satisfies it before the
  // grace expires.
  in.consoleAnswered = true;
  in.uptimeMs = 0;
  TEST_ASSERT_TRUE((OtaDecide::criteriaMet(in, CFG) & OtaDecide::CRIT_CONSOLE) != 0);
}

void test_thresholds_are_inclusive() {
  // >= not >. Exactly minTicks, exactly minUptimeMs and exactly consoleGraceMs
  // all count. An off-by-one here costs one extra tick, which is invisible;
  // the reason to pin it is that the same operator appears in decide()'s window
  // comparison, where being wrong costs a rollback.
  OtaDecide::Inputs in = healthy();
  in.ticks = CFG.minTicks;
  in.uptimeMs = CFG.minUptimeMs;
  TEST_ASSERT_EQUAL_UINT8(OtaDecide::CRIT_ALL, OtaDecide::criteriaMet(in, CFG));
}

// ---- the decision table --------------------------------------------------

void test_not_pending_is_always_idle() {
  // EVERY USB FLASH takes this path: otadata erased, state UNDEFINED. It must
  // be IDLE no matter what the criteria or the clock say — including past the
  // window, where a mishandled "not pending" would trigger a rollback on a
  // device that was never updating anything.
  OtaDecide::Inputs in = healthy();
  TEST_ASSERT_EQUAL(OtaDecide::IDLE, OtaDecide::decide(false, in, CFG));

  in.registryFatal = true;
  in.essentialEnabled = false;
  in.ticks = 0;
  in.uptimeMs = CFG.windowMs * 4;
  TEST_ASSERT_EQUAL(OtaDecide::IDLE, OtaDecide::decide(false, in, CFG));
}

void test_pending_and_healthy_confirms() {
  TEST_ASSERT_EQUAL(OtaDecide::CONFIRM, OtaDecide::decide(true, healthy(), CFG));
}

void test_pending_and_early_waits() {
  OtaDecide::Inputs in = healthy();
  in.uptimeMs = 0;
  in.ticks = 0;
  TEST_ASSERT_EQUAL(OtaDecide::WAIT, OtaDecide::decide(true, in, CFG));

  // Still waiting one millisecond before the uptime gate.
  in = healthy();
  in.uptimeMs = CFG.minUptimeMs - 1;
  TEST_ASSERT_EQUAL(OtaDecide::WAIT, OtaDecide::decide(true, in, CFG));
}

void test_window_expiry_rolls_back_only_with_criteria_outstanding() {
  OtaDecide::Inputs in = healthy();
  in.essentialEnabled = false;  // a criterion that will never recover
  in.uptimeMs = CFG.windowMs;
  TEST_ASSERT_EQUAL(OtaDecide::ROLLBACK, OtaDecide::decide(true, in, CFG));

  // One millisecond earlier it is still waiting, not rolling back.
  in.uptimeMs = CFG.windowMs - 1;
  TEST_ASSERT_EQUAL(OtaDecide::WAIT, OtaDecide::decide(true, in, CFG));
}

void test_confirm_beats_the_window_on_the_same_tick() {
  // ORDER OF THE TABLE, asserted. A healthy image whose criteria complete on
  // the very tick the window closes must CONFIRM, not roll back: the opposite
  // order would discard a good build on a one-tick timing coincidence, and the
  // tick period is 250 ms.
  OtaDecide::Inputs in = healthy();
  in.uptimeMs = CFG.windowMs;
  TEST_ASSERT_EQUAL(OtaDecide::CONFIRM, OtaDecide::decide(true, in, CFG));

  in.uptimeMs = CFG.windowMs * 10;
  TEST_ASSERT_EQUAL(OtaDecide::CONFIRM, OtaDecide::decide(true, in, CFG));
}

void test_a_crash_loop_can_never_confirm_itself() {
  // The reason CRIT_UPTIME exists. An image that panics and reboots inside the
  // uptime gate sees a fresh, small millis() on every attempt and never reaches
  // CONFIRM — so the bootloader gets its rollback at the next restart, which is
  // the whole safety property.
  for (uint32_t boot = 0; boot < CFG.minUptimeMs; boot += CFG.tickMs) {
    OtaDecide::Inputs in = healthy();
    in.uptimeMs = boot;
    in.ticks = boot / CFG.tickMs;
    TEST_ASSERT_NOT_EQUAL(OtaDecide::CONFIRM, OtaDecide::decide(true, in, CFG));
  }
}

void test_verdict_names() {
  TEST_ASSERT_EQUAL_STRING("idle", OtaDecide::verdictName(OtaDecide::IDLE));
  TEST_ASSERT_EQUAL_STRING("wait", OtaDecide::verdictName(OtaDecide::WAIT));
  TEST_ASSERT_EQUAL_STRING("confirm", OtaDecide::verdictName(OtaDecide::CONFIRM));
  TEST_ASSERT_EQUAL_STRING("rollback", OtaDecide::verdictName(OtaDecide::ROLLBACK));
}

// ---- the reported numbers ------------------------------------------------

void test_remaining_saturates_instead_of_wrapping() {
  OtaDecide::Inputs in = healthy();
  in.uptimeMs = 0;
  TEST_ASSERT_EQUAL_UINT32(CFG.windowMs, OtaDecide::remainingMs(in, CFG));

  in.uptimeMs = CFG.windowMs;
  TEST_ASSERT_EQUAL_UINT32(0, OtaDecide::remainingMs(in, CFG));

  // The one that matters: past the window a plain subtraction underflows to
  // ~49 days, and this feeds a "seconds remaining" an operator reads.
  in.uptimeMs = CFG.windowMs + 1;
  TEST_ASSERT_EQUAL_UINT32(0, OtaDecide::remainingMs(in, CFG));
  in.uptimeMs = 0xFFFFFFFFu;
  TEST_ASSERT_EQUAL_UINT32(0, OtaDecide::remainingMs(in, CFG));
}

void test_progress_is_clamped_and_monotonic() {
  OtaDecide::Inputs in = healthy();
  uint8_t last = 0;
  for (uint32_t t = 0; t <= CFG.minUptimeMs; t += CFG.tickMs) {
    in.uptimeMs = t;
    uint8_t pct = OtaDecide::progressPct(in, CFG);
    TEST_ASSERT_TRUE(pct >= last);
    TEST_ASSERT_TRUE(pct <= 100);
    last = pct;
  }
  TEST_ASSERT_EQUAL_UINT8(100, last);

  // Past the gate it pins at 100 rather than overflowing the uint8_t, which a
  // bare (uptime * 100 / min) would do just past 42 s of the multiply.
  in.uptimeMs = 0xFFFFFFFFu;
  TEST_ASSERT_EQUAL_UINT8(100, OtaDecide::progressPct(in, CFG));
}

void test_shipped_timings_are_internally_consistent() {
  // The same relationships the static_asserts in otadecide.h pin at compile
  // time, restated as a runtime test so a failure names the offending pair
  // rather than pointing at a header line.
  TEST_ASSERT_TRUE(CFG.minTicks * CFG.tickMs <= CFG.minUptimeMs);
  TEST_ASSERT_TRUE(CFG.consoleGraceMs <= CFG.minUptimeMs);
  TEST_ASSERT_TRUE(CFG.minUptimeMs < CFG.windowMs);
  // Stuart asked for "order of 20-30 s" on the uptime gate and a generous
  // window. Pinned so a later tweak is a deliberate edit to a test, not a
  // silent drift in a header constant.
  TEST_ASSERT_UINT32_WITHIN(5000, 25000, CFG.minUptimeMs);
  TEST_ASSERT_TRUE(CFG.windowMs >= 60000);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_crit_all_is_exactly_the_five_bits);
  RUN_TEST(test_every_bit_has_a_stable_name);
  RUN_TEST(test_healthy_device_meets_everything);
  RUN_TEST(test_each_criterion_can_fail_on_its_own);
  RUN_TEST(test_console_criterion_is_an_or_not_an_and);
  RUN_TEST(test_thresholds_are_inclusive);
  RUN_TEST(test_not_pending_is_always_idle);
  RUN_TEST(test_pending_and_healthy_confirms);
  RUN_TEST(test_pending_and_early_waits);
  RUN_TEST(test_window_expiry_rolls_back_only_with_criteria_outstanding);
  RUN_TEST(test_confirm_beats_the_window_on_the_same_tick);
  RUN_TEST(test_a_crash_loop_can_never_confirm_itself);
  RUN_TEST(test_verdict_names);
  RUN_TEST(test_remaining_saturates_instead_of_wrapping);
  RUN_TEST(test_progress_is_clamped_and_monotonic);
  RUN_TEST(test_shipped_timings_are_internally_consistent);
  return UNITY_END();
}
