// Host-side unit tests for the built-in command auth policy (cmdauth.h).
//
// `pio test -e native` — no hardware, no transports. This is the whole of
// backlog S1's decision logic: which built-in needs which AuthLevel, whether a
// given session level clears it, and what the refusal says.
//
// It is worth testing here rather than on the device for the obvious reason —
// the failure mode is silent. A command that quietly drops to AUTH_NONE, or a
// gate written as > instead of >=, produces a device that works perfectly and
// answers `reboot` to a stranger. Nothing about that shows up on the LCD or in
// a boot log.
//
// The table under test is the SAME one console.cpp initialises struct Command
// from (via CmdAuth::requiredFor at compile time), so these assertions are
// about the shipped policy, not a copy of it.

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmdauth.h"

void setUp() {}
void tearDown() {}

// ---- levels --------------------------------------------------------------

void test_levels_are_ordered_and_named() {
  // The ordering IS the policy: permits() is >=, so PHYSICAL must outrank
  // TOKEN must outrank NONE. A reordering here silently inverts every gate.
  TEST_ASSERT_TRUE(CmdAuth::NONE < CmdAuth::TOKEN);
  TEST_ASSERT_TRUE(CmdAuth::TOKEN < CmdAuth::PHYSICAL);
  TEST_ASSERT_EQUAL_STRING("none", CmdAuth::levelName(CmdAuth::NONE));
  TEST_ASSERT_EQUAL_STRING("token", CmdAuth::levelName(CmdAuth::TOKEN));
  TEST_ASSERT_EQUAL_STRING("physical", CmdAuth::levelName(CmdAuth::PHYSICAL));
  TEST_ASSERT_EQUAL_STRING("?", CmdAuth::levelName(7));
}

void test_permits_is_cumulative_not_exact() {
  // AUTH_PHYSICAL (the USB cable) must satisfy an AUTH_TOKEN requirement, or
  // the console can no longer run the commands a phone can.
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::PHYSICAL, CmdAuth::TOKEN));
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::PHYSICAL, CmdAuth::PHYSICAL));
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::TOKEN, CmdAuth::TOKEN));
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::TOKEN, CmdAuth::NONE));
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::NONE, CmdAuth::NONE));

  TEST_ASSERT_FALSE(CmdAuth::permits(CmdAuth::TOKEN, CmdAuth::PHYSICAL));
  TEST_ASSERT_FALSE(CmdAuth::permits(CmdAuth::NONE, CmdAuth::TOKEN));
  TEST_ASSERT_FALSE(CmdAuth::permits(CmdAuth::NONE, CmdAuth::PHYSICAL));
}

// ---- the policy table ----------------------------------------------------

void test_reboot_is_physical_only() {
  // stuart's call, 2026-08-17: only someone holding the cable may restart the
  // device. Also closes backlog S2 (`reboot` reachable at AUTH_TOKEN).
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, CmdAuth::requiredFor("reboot"));
  TEST_ASSERT_FALSE(CmdAuth::permits(CmdAuth::TOKEN, CmdAuth::requiredFor("reboot")));
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::PHYSICAL, CmdAuth::requiredFor("reboot")));
}

void test_every_other_builtin_is_token() {
  const char *tokenLevel[] = {"help",     "info",    "parts",    "mem",     "uptime",   "tasks",
                              "led",      "modules", "enable",   "disable", "selftest", "log",
                              "bootprobe",
                              // `ota` READS at token. Its two mutating params
                              // are a separate gate — see the OTA_MUTATE test
                              // below.
                              "ota"};
  for (size_t i = 0; i < sizeof(tokenLevel) / sizeof(tokenLevel[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(CmdAuth::isListed(tokenLevel[i]), tokenLevel[i]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CmdAuth::TOKEN, CmdAuth::requiredFor(tokenLevel[i]), tokenLevel[i]);
  }
  // ...and that list plus `reboot` is the WHOLE table. A command added to
  // cmdauth.h without being considered here fails this.
  TEST_ASSERT_EQUAL_size_t(sizeof(tokenLevel) / sizeof(tokenLevel[0]) + 1, CmdAuth::BUILTIN_COUNT);
}

// The one parameter-level gate among the built-ins (backlog S4). `ota` reads at
// TOKEN so a phone that pushed an update can see whether it stuck; `confirm`
// and `rollback` decide which image this device boots and sit at PHYSICAL
// beside `reboot`.
void test_ota_mutating_params_are_physical() {
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::TOKEN, CmdAuth::requiredFor("ota"));
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, CmdAuth::OTA_MUTATE);
  // A parameter gate may only RAISE. Console::execute()'s central gate has
  // already run by the time the handler sees the request, so a lower value here
  // would be dead code that reads as if it were a gate. (Also a static_assert
  // in cmdauth.h; asserted here so a failure names the invariant.)
  TEST_ASSERT_TRUE(CmdAuth::OTA_MUTATE >= CmdAuth::requiredFor("ota"));
  // A token session may read but not mutate; the cable may do both.
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::TOKEN, CmdAuth::requiredFor("ota")));
  TEST_ASSERT_FALSE(CmdAuth::permits(CmdAuth::TOKEN, CmdAuth::OTA_MUTATE));
  TEST_ASSERT_TRUE(CmdAuth::permits(CmdAuth::PHYSICAL, CmdAuth::OTA_MUTATE));
  TEST_ASSERT_FALSE(CmdAuth::permits(CmdAuth::NONE, CmdAuth::requiredFor("ota")));
}

void test_nothing_is_reachable_at_auth_none() {
  // The property BLE (backlog F1) will rely on: hand this an unauthenticated
  // context and there is nothing for it to run.
  for (size_t i = 0; i < CmdAuth::BUILTIN_COUNT; i++) {
    TEST_ASSERT_FALSE_MESSAGE(CmdAuth::permits(CmdAuth::NONE, CmdAuth::BUILTINS[i].minAuth), CmdAuth::BUILTINS[i].name);
  }
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::TOKEN, CmdAuth::minimumLevel());
}

void test_unlisted_commands_fail_closed() {
  // A built-in added to console.cpp with no policy entry must be refused to
  // every network caller, not opened to them. (console.cpp also static_asserts
  // that this never happens; this is what the behaviour would be.)
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, CmdAuth::requiredFor("wipe"));
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, CmdAuth::requiredFor(""));
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, CmdAuth::requiredFor(nullptr));
  TEST_ASSERT_FALSE(CmdAuth::isListed("wipe"));
  TEST_ASSERT_FALSE(CmdAuth::isListed(nullptr));
}

void test_lookup_is_exact_not_prefix() {
  // "reboo", "reboots" and "REBOOT" are not `reboot`. A prefix match would let
  // a near-miss inherit a level it was never granted.
  TEST_ASSERT_FALSE(CmdAuth::isListed("reboo"));
  TEST_ASSERT_FALSE(CmdAuth::isListed("reboots"));
  TEST_ASSERT_FALSE(CmdAuth::isListed("REBOOT"));
  TEST_ASSERT_FALSE(CmdAuth::isListed("inf"));
  TEST_ASSERT_FALSE(CmdAuth::isListed("infos"));
}

void test_table_is_well_formed() {
  for (size_t i = 0; i < CmdAuth::BUILTIN_COUNT; i++) {
    TEST_ASSERT_NOT_NULL(CmdAuth::BUILTINS[i].name);
    TEST_ASSERT_TRUE(CmdAuth::BUILTINS[i].name[0] != '\0');
    TEST_ASSERT_TRUE(CmdAuth::BUILTINS[i].minAuth <= CmdAuth::PHYSICAL);
    // No duplicates: requiredFor() returns the FIRST match, so a second entry
    // for the same name would be dead policy that reads as if it applied.
    for (size_t j = i + 1; j < CmdAuth::BUILTIN_COUNT; j++) {
      TEST_ASSERT_FALSE_MESSAGE(strcmp(CmdAuth::BUILTINS[i].name, CmdAuth::BUILTINS[j].name) == 0,
                                CmdAuth::BUILTINS[i].name);
    }
  }
}

// ---- the gate, as a decision table --------------------------------------

void test_decision_table_command_by_level() {
  struct Case {
    const char *act;
    uint8_t level;
    bool allowed;
  };
  // This is the table stuart signed off, written out per (command, session)
  // pair rather than derived, so an edit to the policy has to disagree with an
  // explicit expectation rather than with a rule that was edited alongside it.
  const Case cases[] = {
      {"info", CmdAuth::NONE, false},      {"info", CmdAuth::TOKEN, true},      {"info", CmdAuth::PHYSICAL, true},
      {"parts", CmdAuth::NONE, false},     {"parts", CmdAuth::TOKEN, true},     {"parts", CmdAuth::PHYSICAL, true},
      {"bootprobe", CmdAuth::NONE, false}, {"bootprobe", CmdAuth::TOKEN, true}, {"bootprobe", CmdAuth::PHYSICAL, true},
      // enable/disable stay at TOKEN even though `enable hid` arms a keyboard:
      // stuart chose visibility (the LCD badge, backlog C3) over a higher level
      // here, because arming is inert until a reboot the network cannot do.
      {"enable", CmdAuth::NONE, false},    {"enable", CmdAuth::TOKEN, true},    {"enable", CmdAuth::PHYSICAL, true},
      {"disable", CmdAuth::NONE, false},   {"disable", CmdAuth::TOKEN, true},   {"disable", CmdAuth::PHYSICAL, true},
      {"reboot", CmdAuth::NONE, false},    {"reboot", CmdAuth::TOKEN, false},   {"reboot", CmdAuth::PHYSICAL, true},
      // `ota` with no params — READING the rollback state. A phone session may;
      // an unauthenticated one may not.
      {"ota", CmdAuth::NONE, false},       {"ota", CmdAuth::TOKEN, true},       {"ota", CmdAuth::PHYSICAL, true},
      // An unknown command, at every level: only the cable clears the
      // fail-closed default.
      {"nosuchcommand", CmdAuth::NONE, false}, {"nosuchcommand", CmdAuth::TOKEN, false},
      {"nosuchcommand", CmdAuth::PHYSICAL, true},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool got = CmdAuth::permits(cases[i].level, CmdAuth::requiredFor(cases[i].act));
    TEST_ASSERT_EQUAL_MESSAGE(cases[i].allowed, got, cases[i].act);
  }
}

// ---- the refusal ---------------------------------------------------------

void test_deny_message_names_the_command_and_both_levels() {
  char msg[96];  // sizeof(CmdError::msg)
  CmdAuth::denyMessage(msg, sizeof(msg), "the built-in command 'reboot'", CmdAuth::PHYSICAL, "ws", CmdAuth::TOKEN);
  TEST_ASSERT_EQUAL_STRING("the built-in command 'reboot' needs auth >= physical; transport 'ws' is at level 1", msg);
}

void test_deny_message_fits_a_cmderror_buffer_for_every_command() {
  // CmdError::msg is 96 bytes and cmdErrorf() truncates to it. Every refusal
  // this can produce must fit UNCUT, or a caller could tell a built-in's
  // refusal from a module's — or one built-in from another — by its length.
  char what[48];
  char msg[96];
  const char *transports[] = {"cdc", "http", "ws", "ble"};
  for (size_t i = 0; i < CmdAuth::BUILTIN_COUNT; i++) {
    snprintf(what, sizeof(what), "the built-in command '%.16s'", CmdAuth::BUILTINS[i].name);
    for (size_t t = 0; t < sizeof(transports) / sizeof(transports[0]); t++) {
      size_t n = CmdAuth::denyMessage(msg, sizeof(msg), what, CmdAuth::BUILTINS[i].minAuth, transports[t],
                                      CmdAuth::NONE);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(msg), n, CmdAuth::BUILTINS[i].name);
      TEST_ASSERT_TRUE_MESSAGE(n < sizeof(msg) - 1, CmdAuth::BUILTINS[i].name);
    }
  }
  // The pre-flight refusal, which names no command at all.
  size_t n = CmdAuth::denyMessage(msg, sizeof(msg), "any built-in command", CmdAuth::minimumLevel(), "ble",
                                  CmdAuth::NONE);
  TEST_ASSERT_TRUE(n < sizeof(msg) - 1);
  TEST_ASSERT_EQUAL_STRING("any built-in command needs auth >= token; transport 'ble' is at level 0", msg);
}

void test_deny_message_survives_degenerate_input() {
  char msg[96];
  TEST_ASSERT_EQUAL_size_t(0, CmdAuth::denyMessage(nullptr, sizeof(msg), "x", 1, "y", 0));
  TEST_ASSERT_EQUAL_size_t(0, CmdAuth::denyMessage(msg, 0, "x", 1, "y", 0));
  // A null transport is what a CmdContext built by a careless adapter carries.
  CmdAuth::denyMessage(msg, sizeof(msg), nullptr, CmdAuth::TOKEN, nullptr, CmdAuth::NONE);
  TEST_ASSERT_NOT_NULL(strstr(msg, "'?'"));
  TEST_ASSERT_NOT_NULL(strstr(msg, "this command"));
}

void test_deny_message_fits_a_tiny_buffer() {
  char tiny[8];
  size_t n = CmdAuth::denyMessage(tiny, sizeof(tiny), "the built-in command 'reboot'", CmdAuth::PHYSICAL, "ws", 1);
  TEST_ASSERT_EQUAL_size_t(sizeof(tiny) - 1, n);
  TEST_ASSERT_EQUAL_size_t(sizeof(tiny) - 1, strlen(tiny));
}

// ---- the "as" downgrade ---------------------------------------------------
//
// Pure-function coverage of CmdAuth::resolveAs(), the whole decision behind
// the request envelope's optional "as" field — provable here without a
// device, exactly per the file banner's reasoning for why this policy lives
// host-side at all.

void test_as_absent_means_unchanged() {
  CmdAuth::AsResolution r = CmdAuth::resolveAs(nullptr, CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::NO_REQUEST);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, r.effective);

  r = CmdAuth::resolveAs(nullptr, CmdAuth::NONE);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::NO_REQUEST);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::NONE, r.effective);
}

void test_as_below_actual_downgrades() {
  CmdAuth::AsResolution r = CmdAuth::resolveAs("token", CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::OK);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::TOKEN, r.effective);

  r = CmdAuth::resolveAs("none", CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::OK);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::NONE, r.effective);

  r = CmdAuth::resolveAs("none", CmdAuth::TOKEN);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::OK);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::NONE, r.effective);
}

void test_as_equal_to_actual_is_a_noop() {
  const uint8_t levels[] = {CmdAuth::NONE, CmdAuth::TOKEN, CmdAuth::PHYSICAL};
  for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
    CmdAuth::AsResolution r = CmdAuth::resolveAs(CmdAuth::levelName(levels[i]), levels[i]);
    TEST_ASSERT_TRUE_MESSAGE(r.outcome == CmdAuth::AsOutcome::OK, CmdAuth::levelName(levels[i]));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(levels[i], r.effective, CmdAuth::levelName(levels[i]));
  }
}

// The property that matters most: asking for MORE than you hold is a clear
// error and NEVER clamps upward. Checked both by outcome and, separately, by
// asserting `effective` itself never exceeds `actual` — the min() in
// resolveAs() is meant to make that true unconditionally, not just on the
// paths that also check `outcome`.
void test_as_above_actual_errors_and_never_clamps_up() {
  CmdAuth::AsResolution r = CmdAuth::resolveAs("physical", CmdAuth::TOKEN);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::ESCALATION);
  TEST_ASSERT_TRUE(r.effective <= CmdAuth::TOKEN);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::TOKEN, r.effective);  // left at actual, not raised

  r = CmdAuth::resolveAs("physical", CmdAuth::NONE);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::ESCALATION);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::NONE, r.effective);

  r = CmdAuth::resolveAs("token", CmdAuth::NONE);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::ESCALATION);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::NONE, r.effective);
}

// The property proved exhaustively over the whole (requested, actual) grid,
// including the unparseable and absent cases: `effective` can never exceed
// `actual`, by construction, independent of whether the caller also checks
// `outcome`.
void test_as_effective_never_exceeds_actual_for_any_input() {
  const char *requests[] = {nullptr, "", "bogus", "NONE", "none", "token", "physical"};
  const uint8_t levels[] = {CmdAuth::NONE, CmdAuth::TOKEN, CmdAuth::PHYSICAL};
  for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
    for (size_t j = 0; j < sizeof(levels) / sizeof(levels[0]); j++) {
      CmdAuth::AsResolution r = CmdAuth::resolveAs(requests[i], levels[j]);
      TEST_ASSERT_TRUE(r.effective <= levels[j]);
    }
  }
}

void test_as_unparseable_value_is_an_error() {
  CmdAuth::AsResolution r = CmdAuth::resolveAs("bogus", CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::UNPARSEABLE);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, r.effective);

  // Case-sensitive, exact match only — matching isListed()'s lookup style
  // elsewhere in this file. "Token" and " token" are not "token".
  r = CmdAuth::resolveAs("Token", CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::UNPARSEABLE);
  r = CmdAuth::resolveAs(" token", CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::UNPARSEABLE);
  r = CmdAuth::resolveAs("", CmdAuth::PHYSICAL);
  TEST_ASSERT_TRUE(r.outcome == CmdAuth::AsOutcome::UNPARSEABLE);
}

void test_parse_level_round_trips_level_name() {
  const uint8_t levels[] = {CmdAuth::NONE, CmdAuth::TOKEN, CmdAuth::PHYSICAL};
  for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
    uint8_t out = 255;
    TEST_ASSERT_TRUE(CmdAuth::parseLevel(CmdAuth::levelName(levels[i]), &out));
    TEST_ASSERT_EQUAL_UINT8(levels[i], out);
  }
  uint8_t out = 255;
  TEST_ASSERT_FALSE(CmdAuth::parseLevel("bogus", &out));
  TEST_ASSERT_FALSE(CmdAuth::parseLevel(nullptr, &out));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_levels_are_ordered_and_named);
  RUN_TEST(test_permits_is_cumulative_not_exact);
  RUN_TEST(test_reboot_is_physical_only);
  RUN_TEST(test_every_other_builtin_is_token);
  RUN_TEST(test_ota_mutating_params_are_physical);
  RUN_TEST(test_nothing_is_reachable_at_auth_none);
  RUN_TEST(test_unlisted_commands_fail_closed);
  RUN_TEST(test_lookup_is_exact_not_prefix);
  RUN_TEST(test_table_is_well_formed);
  RUN_TEST(test_decision_table_command_by_level);
  RUN_TEST(test_deny_message_names_the_command_and_both_levels);
  RUN_TEST(test_deny_message_fits_a_cmderror_buffer_for_every_command);
  RUN_TEST(test_deny_message_survives_degenerate_input);
  RUN_TEST(test_deny_message_fits_a_tiny_buffer);
  RUN_TEST(test_as_absent_means_unchanged);
  RUN_TEST(test_as_below_actual_downgrades);
  RUN_TEST(test_as_equal_to_actual_is_a_noop);
  RUN_TEST(test_as_above_actual_errors_and_never_clamps_up);
  RUN_TEST(test_as_effective_never_exceeds_actual_for_any_input);
  RUN_TEST(test_as_unparseable_value_is_an_error);
  RUN_TEST(test_parse_level_round_trips_level_name);
  return UNITY_END();
}
