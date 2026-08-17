// Host-side unit tests for ScreenFmt and Dirty (screenfmt.h).
//
// `pio test -e native` — no hardware, no panel. These cover the two parts of
// the LCD module that can be wrong without anyone noticing on a bench:
//
//   * TEXT FITTING. Adafruit_GFX does not clip; an overlong string wraps or
//     draws past the region boundary into the region below, corrupting a
//     region the dirty tracker still believes is valid. An SSID is up to 32
//     characters and is not ours. So every fit() boundary is asserted here
//     rather than eyeballed on glass.
//   * DIRTY TRACKING. takeNext() hands regions out round-robin specifically so
//     that a region which re-dirties every frame (the activity animation)
//     cannot starve the ones below it, given a per-tick draw cap. That is a
//     scheduling property; it is invisible on a screenshot and obvious in a
//     test.

#include <unity.h>

#include <string.h>

#include "screenfmt.h"

void setUp() {}
void tearDown() {}

// ---- ScreenFmt::fit -----------------------------------------------------

void test_fit_passes_short_strings_through() {
  char out[32];
  TEST_ASSERT_EQUAL_size_t(5, ScreenFmt::fit(out, sizeof(out), "hello", 26));
  TEST_ASSERT_EQUAL_STRING("hello", out);
}

void test_fit_exact_length_is_not_truncated() {
  char out[32];
  // Exactly the budget: no mark, nothing lost. The off-by-one that matters.
  TEST_ASSERT_EQUAL_size_t(5, ScreenFmt::fit(out, sizeof(out), "abcde", 5));
  TEST_ASSERT_EQUAL_STRING("abcde", out);
}

void test_fit_one_over_budget_truncates_with_mark() {
  char out[32];
  TEST_ASSERT_EQUAL_size_t(5, ScreenFmt::fit(out, sizeof(out), "abcdef", 5));
  TEST_ASSERT_EQUAL_STRING("abcd~", out);
}

void test_fit_truncates_a_hostile_ssid_to_the_panel_width() {
  char out[32];
  const char *ssid = "aVeryLongNetworkNameThatGoesOnAndOn";
  TEST_ASSERT_EQUAL_size_t(26, ScreenFmt::fit(out, sizeof(out), ssid, 26));
  TEST_ASSERT_EQUAL_size_t(26, strlen(out));
  TEST_ASSERT_EQUAL_CHAR('~', out[25]);
}

void test_fit_is_bounded_by_the_buffer_not_just_the_columns() {
  char out[6];  // 5 usable columns, whatever the caller claims
  TEST_ASSERT_EQUAL_size_t(5, ScreenFmt::fit(out, sizeof(out), "abcdefghij", 26));
  TEST_ASSERT_EQUAL_STRING("abcd~", out);
}

void test_fit_handles_degenerate_inputs() {
  char out[8];
  memset(out, 'X', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, ScreenFmt::fit(out, sizeof(out), "abc", 0));
  TEST_ASSERT_EQUAL_STRING("", out);

  memset(out, 'X', sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, ScreenFmt::fit(out, sizeof(out), nullptr, 4));
  TEST_ASSERT_EQUAL_STRING("", out);

  TEST_ASSERT_EQUAL_size_t(0, ScreenFmt::fit(nullptr, 8, "abc", 4));
  TEST_ASSERT_EQUAL_size_t(0, ScreenFmt::fit(out, 0, "abc", 4));

  // One column left: the mark alone is still the truth ("there was more").
  TEST_ASSERT_EQUAL_size_t(1, ScreenFmt::fit(out, sizeof(out), "abc", 1));
  TEST_ASSERT_EQUAL_STRING("~", out);
}

// ---- ScreenFmt::fitPrefixed ---------------------------------------------

void test_fitprefixed_keeps_the_label_and_cuts_the_body() {
  char out[32];
  // 12 columns for "AP " + an 18-character SSID: the label must survive, since
  // it is what says WHAT the truncated thing is.
  TEST_ASSERT_EQUAL_size_t(12, ScreenFmt::fitPrefixed(out, sizeof(out), "AP ", "MyOfficeNetwork123", 12));
  TEST_ASSERT_EQUAL_STRING("AP MyOffice~", out);
}

void test_fitprefixed_fits_when_everything_fits() {
  char out[32];
  ScreenFmt::fitPrefixed(out, sizeof(out), "AP ", "tdongle-a9d8", 26);
  TEST_ASSERT_EQUAL_STRING("AP tdongle-a9d8", out);
}

void test_fitprefixed_survives_a_prefix_longer_than_the_line() {
  char out[32];
  TEST_ASSERT_EQUAL_size_t(4, ScreenFmt::fitPrefixed(out, sizeof(out), "prefix", "body", 4));
  TEST_ASSERT_EQUAL_STRING("pre~", out);
}

void test_fitprefixed_with_empty_body_is_just_the_prefix() {
  char out[32];
  TEST_ASSERT_EQUAL_size_t(3, ScreenFmt::fitPrefixed(out, sizeof(out), "AP ", "", 26));
  TEST_ASSERT_EQUAL_STRING("AP ", out);
}

// ---- ScreenFmt::uptime / compactBytes -----------------------------------

void test_uptime_formats_hms_then_days() {
  char out[16];
  ScreenFmt::uptime(out, sizeof(out), 0);
  TEST_ASSERT_EQUAL_STRING("00:00:00", out);
  ScreenFmt::uptime(out, sizeof(out), 3661u * 1000u);
  TEST_ASSERT_EQUAL_STRING("01:01:01", out);
  ScreenFmt::uptime(out, sizeof(out), 86399u * 1000u);
  TEST_ASSERT_EQUAL_STRING("23:59:59", out);
  ScreenFmt::uptime(out, sizeof(out), (86400u + 3600u * 2u + 60u * 3u) * 1000u);
  TEST_ASSERT_EQUAL_STRING("1d02:03", out);
}

void test_uptime_stays_inside_a_short_buffer() {
  char out[5];
  size_t n = ScreenFmt::uptime(out, sizeof(out), 3661u * 1000u);
  TEST_ASSERT_TRUE(n < sizeof(out));
  TEST_ASSERT_EQUAL_size_t(strlen(out), n);
}

void test_compactbytes_picks_a_sensible_unit() {
  char out[12];
  ScreenFmt::compactBytes(out, sizeof(out), 832);
  TEST_ASSERT_EQUAL_STRING("832", out);
  ScreenFmt::compactBytes(out, sizeof(out), 1024);
  TEST_ASSERT_EQUAL_STRING("1k", out);
  ScreenFmt::compactBytes(out, sizeof(out), 155648);  // 152 KiB
  TEST_ASSERT_EQUAL_STRING("152k", out);
  ScreenFmt::compactBytes(out, sizeof(out), 1024u * 1024u);
  TEST_ASSERT_EQUAL_STRING("1.0M", out);
  // Truncating, not rounding: 1.99 MiB must not read as "2.0M".
  ScreenFmt::compactBytes(out, sizeof(out), 2u * 1024u * 1024u - 1u);
  TEST_ASSERT_EQUAL_STRING("1.9M", out);
}

// ---- Dirty --------------------------------------------------------------

void test_dirty_starts_clean_and_marks() {
  Dirty::Regions r;
  Dirty::init(r, 6);
  TEST_ASSERT_FALSE(Dirty::any(r));
  TEST_ASSERT_EQUAL_UINT8(6, Dirty::takeNext(r));  // "none" == count

  Dirty::mark(r, 3);
  TEST_ASSERT_TRUE(Dirty::any(r));
  TEST_ASSERT_TRUE(Dirty::isDirty(r, 3));
  TEST_ASSERT_FALSE(Dirty::isDirty(r, 2));
  TEST_ASSERT_EQUAL_UINT8(1, Dirty::pending(r));
}

void test_dirty_ignores_out_of_range_regions() {
  Dirty::Regions r;
  Dirty::init(r, 6);
  Dirty::mark(r, 6);
  Dirty::mark(r, 200);
  TEST_ASSERT_FALSE(Dirty::any(r));
}

void test_dirty_markall_and_clearall() {
  Dirty::Regions r;
  Dirty::init(r, 6);
  Dirty::markAll(r);
  TEST_ASSERT_EQUAL_UINT8(6, Dirty::pending(r));
  Dirty::clearAll(r);
  TEST_ASSERT_FALSE(Dirty::any(r));
}

void test_dirty_takenext_clears_what_it_returns() {
  Dirty::Regions r;
  Dirty::init(r, 6);
  Dirty::mark(r, 2);
  TEST_ASSERT_EQUAL_UINT8(2, Dirty::takeNext(r));
  TEST_ASSERT_FALSE(Dirty::isDirty(r, 2));
  TEST_ASSERT_EQUAL_UINT8(6, Dirty::takeNext(r));
}

void test_dirty_takenext_is_round_robin_not_lowest_first() {
  // THE STARVATION TEST. Region 0 goes dirty every frame (this is the activity
  // animation) and only ONE region is drawn per frame. Lowest-index-first
  // would return 0 forever and region 5 would never be repainted.
  Dirty::Regions r;
  Dirty::init(r, 6);
  Dirty::mark(r, 0);
  Dirty::mark(r, 5);

  bool sawFive = false;
  for (int frame = 0; frame < 6 && !sawFive; frame++) {
    Dirty::mark(r, 0);  // re-dirtied every frame
    uint8_t got = Dirty::takeNext(r);
    if (got == 5) {
      sawFive = true;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(sawFive, "a constantly-dirty region starved a quiet one");
}

void test_dirty_takenext_wraps_the_cursor() {
  Dirty::Regions r;
  Dirty::init(r, 4);
  Dirty::markAll(r);
  // Four takes must hand back all four distinct regions exactly once.
  bool seen[4] = {false, false, false, false};
  for (int i = 0; i < 4; i++) {
    uint8_t got = Dirty::takeNext(r);
    TEST_ASSERT_TRUE(got < 4);
    TEST_ASSERT_FALSE(seen[got]);
    seen[got] = true;
  }
  TEST_ASSERT_FALSE(Dirty::any(r));
  TEST_ASSERT_EQUAL_UINT8(4, Dirty::takeNext(r));
}

void test_dirty_zero_regions_is_inert() {
  Dirty::Regions r;
  Dirty::init(r, 0);
  Dirty::markAll(r);
  TEST_ASSERT_FALSE(Dirty::any(r));
  TEST_ASSERT_EQUAL_UINT8(0, Dirty::takeNext(r));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_fit_passes_short_strings_through);
  RUN_TEST(test_fit_exact_length_is_not_truncated);
  RUN_TEST(test_fit_one_over_budget_truncates_with_mark);
  RUN_TEST(test_fit_truncates_a_hostile_ssid_to_the_panel_width);
  RUN_TEST(test_fit_is_bounded_by_the_buffer_not_just_the_columns);
  RUN_TEST(test_fit_handles_degenerate_inputs);
  RUN_TEST(test_fitprefixed_keeps_the_label_and_cuts_the_body);
  RUN_TEST(test_fitprefixed_fits_when_everything_fits);
  RUN_TEST(test_fitprefixed_survives_a_prefix_longer_than_the_line);
  RUN_TEST(test_fitprefixed_with_empty_body_is_just_the_prefix);
  RUN_TEST(test_uptime_formats_hms_then_days);
  RUN_TEST(test_uptime_stays_inside_a_short_buffer);
  RUN_TEST(test_compactbytes_picks_a_sensible_unit);
  RUN_TEST(test_dirty_starts_clean_and_marks);
  RUN_TEST(test_dirty_ignores_out_of_range_regions);
  RUN_TEST(test_dirty_markall_and_clearall);
  RUN_TEST(test_dirty_takenext_clears_what_it_returns);
  RUN_TEST(test_dirty_takenext_is_round_robin_not_lowest_first);
  RUN_TEST(test_dirty_takenext_wraps_the_cursor);
  RUN_TEST(test_dirty_zero_regions_is_inert);
  return UNITY_END();
}
