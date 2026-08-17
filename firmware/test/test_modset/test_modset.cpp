// Host-side unit tests for the persisted module sets and the boot decision
// (modset.h).
//
// `pio test -e native`. This is the rule that decides which modules come up,
// and every one of its cases is a bug that has either bitten this project or
// would be invisible if it did:
//
//   * `display` shipped with defaultEnabled = true and came up DISABLED on
//     stuart's dongle, because "absent from the persisted set" was the only
//     state a module could be in and it meant "off". A newly added default-on
//     module could therefore never turn itself on. That is the first test.
//   * The fix must NOT resurrect anything the owner turned off, which is the
//     opposite failure and a far worse one — the LED coming back is cosmetic,
//     `hid` coming back is a keystroke injector nobody armed.
//   * A persisted id this firmware no longer builds must change nothing.
//   * The list writer must refuse to truncate: a half-written persisted set is
//     modules that quietly stop coming back after a reboot.

#include <unity.h>

#include <string.h>

#include "modset.h"

void setUp() {}
void tearDown() {}

// ---- contains: the shared parser ----------------------------------------

void test_contains_matches_whole_tokens_only() {
  const char *list = "cdc,led,storage";
  TEST_ASSERT_TRUE(ModSet::contains(list, "cdc"));
  TEST_ASSERT_TRUE(ModSet::contains(list, "led"));
  TEST_ASSERT_TRUE(ModSet::contains(list, "storage"));
  // A prefix, an extension and a substring are all NOT the id. `hid` must not
  // be matched by a persisted "hidden", in either direction.
  TEST_ASSERT_FALSE(ModSet::contains(list, "le"));
  TEST_ASSERT_FALSE(ModSet::contains(list, "ledx"));
  TEST_ASSERT_FALSE(ModSet::contains(list, "stor"));
  TEST_ASSERT_FALSE(ModSet::contains("hidden", "hid"));
  TEST_ASSERT_FALSE(ModSet::contains("hid", "hidden"));
}

void test_contains_handles_edges() {
  TEST_ASSERT_FALSE(ModSet::contains(nullptr, "led"));
  TEST_ASSERT_FALSE(ModSet::contains("", "led"));
  TEST_ASSERT_FALSE(ModSet::contains("led", nullptr));
  TEST_ASSERT_FALSE(ModSet::contains("led", ""));
  TEST_ASSERT_TRUE(ModSet::contains("led", "led"));       // single entry
  TEST_ASSERT_TRUE(ModSet::contains("a,led,b", "led"));   // middle
  TEST_ASSERT_TRUE(ModSet::contains("a,b,led", "led"));   // last
  TEST_ASSERT_TRUE(ModSet::contains("led,,b", "b"));      // survives an empty entry
}

// ---- the boot decision ---------------------------------------------------

// A virgin device: no key at all. Every module takes its own default.
void test_first_boot_applies_every_descriptor_default() {
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(nullptr, nullptr, "led", true));
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(nullptr, nullptr, "hid", false));
}

// THE DEFECT. `display` is added to the firmware; the device has a persisted
// set from before it existed. It is absent from the enabled set AND absent
// from the known set, so it is new, so it gets its default.
void test_a_newly_added_module_gets_its_default() {
  const char *on = "cdc,led";
  const char *known = "cdc,led,storage";
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(on, known, "display", true));
  // ...and a new module that is default-OFF still stays off. The rule applies
  // the descriptor's answer, not "on".
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, known, "hid", false));
}

// THE OPPOSITE FAILURE, and the one that matters more. The owner turned `led`
// off: it is in the known set and absent from the enabled set. Its default is
// true and must be ignored, on this boot and every boot after it.
void test_an_explicitly_disabled_module_stays_off() {
  const char *on = "cdc";
  const char *known = "cdc,led,display";
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, known, "led", true));
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, known, "display", true));
  // Still off after the "restore" has been written back and read again — the
  // known set is unchanged by a firmware that registers the same modules.
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, known, "led", true));
}

void test_an_enabled_module_comes_back() {
  TEST_ASSERT_TRUE(ModSet::wantAtBoot("cdc,led,http", "cdc,led,http,display", "http", false));
  TEST_ASSERT_TRUE(ModSet::wantAtBoot("cdc,led,http", "cdc,led,http,display", "led", true));
}

// MIGRATION. A device upgraded from a build that never wrote a known set:
// nothing can be told apart, so nothing is defaulted back on. Conservative on
// purpose — see the migration note in modset.h. The cost is that a default-on
// module added in the SAME upgrade stays off for exactly one boot.
void test_an_absent_known_set_resurrects_nothing() {
  const char *on = "cdc";
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, nullptr, "led", true));
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, nullptr, "display", true));
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(on, nullptr, "cdc", false));
}

// An empty enabled set is a legitimate stored value — the owner turned
// everything off — and is NOT the same as an absent one.
void test_an_empty_enabled_set_is_not_a_first_boot() {
  TEST_ASSERT_FALSE(ModSet::wantAtBoot("", "cdc,led,display", "led", true));
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(nullptr, "cdc,led,display", "led", true));
}

// A persisted id this firmware no longer builds is carried along and changes
// nothing about the modules that do exist. (The registry reports it; the
// decision must not so much as notice it.)
void test_an_unknown_persisted_id_is_tolerated() {
  const char *on = "cdc,wifiscan,led";      // wifiscan is not built in this image
  const char *known = "cdc,wifiscan,led,display";
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(on, known, "led", true));
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(on, known, "cdc", true));
  TEST_ASSERT_FALSE(ModSet::wantAtBoot(on, known, "display", true));  // known, disabled
  TEST_ASSERT_TRUE(ModSet::wantAtBoot(on, known, "blescan", true));   // new
}

// ---- the writer ----------------------------------------------------------

void test_append_builds_a_comma_list() {
  char buf[32];
  buf[0] = '\0';
  TEST_ASSERT_TRUE(ModSet::append(buf, sizeof(buf), "cdc"));
  TEST_ASSERT_EQUAL_STRING("cdc", buf);
  TEST_ASSERT_TRUE(ModSet::append(buf, sizeof(buf), "led"));
  TEST_ASSERT_EQUAL_STRING("cdc,led", buf);
  TEST_ASSERT_TRUE(ModSet::append(buf, sizeof(buf), "display"));
  TEST_ASSERT_EQUAL_STRING("cdc,led,display", buf);
  // Round-trips through the reader it shares a format with.
  TEST_ASSERT_TRUE(ModSet::contains(buf, "display"));
  TEST_ASSERT_TRUE(ModSet::contains(buf, "cdc"));
}

// A silent truncation here is modules that stop coming back after a reboot,
// with nothing anywhere reporting it. The buffer must be left ALONE so the
// caller can report the failure instead of persisting a half-list.
void test_append_refuses_rather_than_truncating() {
  char buf[8];
  strcpy(buf, "cdc");
  TEST_ASSERT_FALSE(ModSet::append(buf, sizeof(buf), "storage"));
  TEST_ASSERT_EQUAL_STRING("cdc", buf);
  TEST_ASSERT_TRUE(ModSet::append(buf, sizeof(buf), "led"));  // exactly fits: "cdc,led" + NUL
  TEST_ASSERT_EQUAL_STRING("cdc,led", buf);
  TEST_ASSERT_FALSE(ModSet::append(buf, sizeof(buf), "x"));
  TEST_ASSERT_EQUAL_STRING("cdc,led", buf);
}

void test_append_edges() {
  char buf[8];
  buf[0] = '\0';
  TEST_ASSERT_FALSE(ModSet::append(buf, sizeof(buf), nullptr));
  TEST_ASSERT_FALSE(ModSet::append(buf, sizeof(buf), ""));
  TEST_ASSERT_FALSE(ModSet::append(nullptr, 8, "led"));
  TEST_ASSERT_FALSE(ModSet::append(buf, 0, "led"));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

// The registry's real worst case: 12 ids of 15 characters must fit
// PERSIST_BUF_SIZE (192) exactly, for BOTH the enabled and the known set.
// registry.h static_asserts the arithmetic; this proves the writer agrees.
void test_a_full_module_set_fits_the_persist_buffer() {
  char buf[192];
  buf[0] = '\0';
  char id[16];
  for (int i = 0; i < 12; i++) {
    memset(id, 'a' + i, 15);
    id[15] = '\0';
    TEST_ASSERT_TRUE_MESSAGE(ModSet::append(buf, sizeof(buf), id), "PERSIST_BUF_SIZE cannot hold a full module set");
  }
  TEST_ASSERT_EQUAL_UINT32(191, (uint32_t)strlen(buf));  // 12*15 + 11 commas
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_contains_matches_whole_tokens_only);
  RUN_TEST(test_contains_handles_edges);
  RUN_TEST(test_first_boot_applies_every_descriptor_default);
  RUN_TEST(test_a_newly_added_module_gets_its_default);
  RUN_TEST(test_an_explicitly_disabled_module_stays_off);
  RUN_TEST(test_an_enabled_module_comes_back);
  RUN_TEST(test_an_absent_known_set_resurrects_nothing);
  RUN_TEST(test_an_empty_enabled_set_is_not_a_first_boot);
  RUN_TEST(test_an_unknown_persisted_id_is_tolerated);
  RUN_TEST(test_append_builds_a_comma_list);
  RUN_TEST(test_append_refuses_rather_than_truncating);
  RUN_TEST(test_append_edges);
  RUN_TEST(test_a_full_module_set_fits_the_persist_buffer);
  return UNITY_END();
}
