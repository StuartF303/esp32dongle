// Host-side unit tests for the Activity state machine (activity.h).
//
// `pio test -e native` — no hardware, no LCD. What is being protected here is
// the CONTRACT between a producer that knows nothing about the display and a
// consumer that knows nothing about the producer:
//
//   * unsubscribed == genuinely nothing recorded. If this regressed, every
//     module's progress reporting would keep a live copy of a job label in RAM
//     on a device whose LCD is off, and the "cheap no-op" claim in the header
//     would be a comment rather than a fact.
//   * seq() only moves on a REAL change. The display polls it every 40 ms tick
//     and repaints when it moves; a seq that ticks on every identical
//     progress() call would repaint the panel continuously and eat the
//     cooperative scheduler's budget.
//   * bounds. who/verb are truncated, never overrun, and pct is clamped —
//     a reporting call must not be able to fail or to corrupt anything.

#include <unity.h>

#include <string.h>

#include "activity.h"

void setUp() {
  // Each test starts from a known state; subscribe(false) is also the reset.
  Activity::subscribe(false);
}

void tearDown() { Activity::subscribe(false); }

// ---- the no-op property -------------------------------------------------

void test_unsubscribed_records_nothing() {
  Activity::Snapshot s;
  Activity::begin("storage", "verify");
  Activity::progress(50);
  Activity::end(true);
  TEST_ASSERT_FALSE(Activity::snapshot(s));
  TEST_ASSERT_EQUAL_UINT8(Activity::IDLE, s.state);
  TEST_ASSERT_EQUAL_STRING("", s.who);
  TEST_ASSERT_EQUAL_STRING("", s.verb);
}

void test_unsubscribing_drops_an_in_flight_job() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  Activity::progress(40);
  Activity::Snapshot s;
  TEST_ASSERT_TRUE(Activity::snapshot(s));

  // The LCD was turned off mid-job: turning it back on must not resurrect it.
  Activity::subscribe(false);
  Activity::subscribe(true);
  TEST_ASSERT_FALSE(Activity::snapshot(s));
  TEST_ASSERT_EQUAL_UINT8(Activity::IDLE, s.state);
}

// ---- the happy path -----------------------------------------------------

void test_begin_progress_end_ok() {
  Activity::subscribe(true);
  Activity::Snapshot s;

  Activity::begin("storage", "verify");
  TEST_ASSERT_TRUE(Activity::snapshot(s));
  TEST_ASSERT_EQUAL_UINT8(Activity::RUNNING, s.state);
  TEST_ASSERT_EQUAL_STRING("storage", s.who);
  TEST_ASSERT_EQUAL_STRING("verify", s.verb);
  TEST_ASSERT_EQUAL_UINT8(0, s.pct);

  Activity::progress(37);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(37, s.pct);

  Activity::end(true);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(Activity::DONE_OK, s.state);
  TEST_ASSERT_EQUAL_UINT8(100, s.pct);  // success completes the bar
}

void test_end_false_is_distinguishable_and_keeps_the_percentage() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  Activity::progress(62);
  Activity::end(false);

  Activity::Snapshot s;
  TEST_ASSERT_TRUE(Activity::snapshot(s));
  TEST_ASSERT_EQUAL_UINT8(Activity::DONE_FAIL, s.state);
  // A failure must NOT be rendered as a full bar. "Stopped at 62%" is the
  // information; "100%" would look exactly like success.
  TEST_ASSERT_EQUAL_UINT8(62, s.pct);
}

// ---- seq(), which is what drives the repaint ----------------------------

void test_seq_moves_only_on_a_real_change() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");

  uint32_t before = Activity::seq();
  Activity::progress(10);
  uint32_t afterChange = Activity::seq();
  TEST_ASSERT_NOT_EQUAL(before, afterChange);

  // The producer calls progress() on every one of its own ticks with the same
  // value for hundreds of ticks. None of those may cause a repaint.
  for (int i = 0; i < 100; i++) {
    Activity::progress(10);
  }
  TEST_ASSERT_EQUAL_UINT32(afterChange, Activity::seq());
}

void test_seq_moves_on_begin_and_end() {
  Activity::subscribe(true);
  uint32_t a = Activity::seq();
  Activity::begin("storage", "verify");
  uint32_t b = Activity::seq();
  TEST_ASSERT_NOT_EQUAL(a, b);
  Activity::end(true);
  TEST_ASSERT_NOT_EQUAL(b, Activity::seq());
}

// ---- misuse, which must be harmless -------------------------------------

void test_progress_without_begin_is_ignored() {
  Activity::subscribe(true);
  Activity::progress(50);
  Activity::Snapshot s;
  TEST_ASSERT_FALSE(Activity::snapshot(s));
}

void test_end_without_begin_is_ignored() {
  Activity::subscribe(true);
  Activity::end(true);
  Activity::Snapshot s;
  TEST_ASSERT_FALSE(Activity::snapshot(s));
}

void test_double_end_does_not_move_seq_again() {
  // The storage module's cancel path can reach finishVerify() twice in
  // pathological orderings; the second end() must be inert.
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  Activity::end(false);
  uint32_t after = Activity::seq();
  Activity::end(true);
  TEST_ASSERT_EQUAL_UINT32(after, Activity::seq());
  Activity::Snapshot s;
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(Activity::DONE_FAIL, s.state);
}

void test_progress_after_end_is_ignored() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  Activity::end(true);
  Activity::progress(3);
  Activity::Snapshot s;
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(100, s.pct);
}

void test_second_begin_replaces_the_first() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  Activity::progress(80);
  Activity::begin("ota", "write");
  Activity::Snapshot s;
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(Activity::RUNNING, s.state);
  TEST_ASSERT_EQUAL_STRING("ota", s.who);
  TEST_ASSERT_EQUAL_STRING("write", s.verb);
  TEST_ASSERT_EQUAL_UINT8(0, s.pct);  // the new job starts at zero, not at 80
}

// ---- bounds -------------------------------------------------------------

void test_progress_is_clamped() {
  Activity::subscribe(true);
  Activity::begin("x", "y");
  Activity::progress(200);
  Activity::Snapshot s;
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(100, s.pct);
}

void test_labels_truncate_rather_than_overrun() {
  Activity::subscribe(true);
  Activity::begin("a-module-id-far-too-long-for-the-panel", "an-extremely-long-verb-indeed");
  Activity::Snapshot s;
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_size_t(Activity::MAX_WHO, strlen(s.who));
  TEST_ASSERT_EQUAL_size_t(Activity::MAX_VERB, strlen(s.verb));
  TEST_ASSERT_EQUAL_STRING("a-module-id-", s.who);
}

void test_null_labels_are_tolerated() {
  Activity::subscribe(true);
  Activity::begin(nullptr, nullptr);
  Activity::Snapshot s;
  TEST_ASSERT_TRUE(Activity::snapshot(s));
  TEST_ASSERT_EQUAL_STRING("", s.who);
  TEST_ASSERT_EQUAL_STRING("", s.verb);
}

void test_progressbytes_computes_and_survives_big_files() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  Activity::Snapshot s;

  Activity::progressBytes(0, 1000);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(0, s.pct);

  Activity::progressBytes(250, 1000);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(25, s.pct);

  // 3 GB file: done * 100 overflows uint32_t at ~42 MB, so this is the case
  // the 64-bit intermediate exists for.
  Activity::progressBytes(1500u * 1024u * 1024u, 3000u * 1024u * 1024u);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(50, s.pct);

  // total == 0 is a real case (an empty file) and means "finished", not a
  // division by zero.
  Activity::progressBytes(0, 0);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(100, s.pct);

  // done > total must not exceed 100.
  Activity::progressBytes(500, 100);
  Activity::snapshot(s);
  TEST_ASSERT_EQUAL_UINT8(100, s.pct);
}

// ---- the consumer-side clear --------------------------------------------

void test_clear_only_affects_a_finished_job() {
  Activity::subscribe(true);
  Activity::begin("storage", "verify");
  uint32_t running = Activity::seq();
  Activity::clear();  // must NOT cancel a running job
  TEST_ASSERT_EQUAL_UINT32(running, Activity::seq());
  Activity::Snapshot s;
  TEST_ASSERT_TRUE(Activity::snapshot(s));
  TEST_ASSERT_EQUAL_UINT8(Activity::RUNNING, s.state);

  Activity::end(true);
  Activity::clear();
  TEST_ASSERT_FALSE(Activity::snapshot(s));

  uint32_t idle = Activity::seq();
  Activity::clear();  // idempotent
  TEST_ASSERT_EQUAL_UINT32(idle, Activity::seq());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_unsubscribed_records_nothing);
  RUN_TEST(test_unsubscribing_drops_an_in_flight_job);
  RUN_TEST(test_begin_progress_end_ok);
  RUN_TEST(test_end_false_is_distinguishable_and_keeps_the_percentage);
  RUN_TEST(test_seq_moves_only_on_a_real_change);
  RUN_TEST(test_seq_moves_on_begin_and_end);
  RUN_TEST(test_progress_without_begin_is_ignored);
  RUN_TEST(test_end_without_begin_is_ignored);
  RUN_TEST(test_double_end_does_not_move_seq_again);
  RUN_TEST(test_progress_after_end_is_ignored);
  RUN_TEST(test_second_begin_replaces_the_first);
  RUN_TEST(test_progress_is_clamped);
  RUN_TEST(test_labels_truncate_rather_than_overrun);
  RUN_TEST(test_null_labels_are_tolerated);
  RUN_TEST(test_progressbytes_computes_and_survives_big_files);
  RUN_TEST(test_clear_only_affects_a_finished_job);
  return UNITY_END();
}
