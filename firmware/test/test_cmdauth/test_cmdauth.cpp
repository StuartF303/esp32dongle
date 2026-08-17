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
  const char *tokenLevel[] = {"help",     "info",    "parts",    "mem", "uptime",   "tasks",
                              "led",      "modules", "enable",   "disable", "selftest", "log",
                              "bootprobe"};
  for (size_t i = 0; i < sizeof(tokenLevel) / sizeof(tokenLevel[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(CmdAuth::isListed(tokenLevel[i]), tokenLevel[i]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CmdAuth::TOKEN, CmdAuth::requiredFor(tokenLevel[i]), tokenLevel[i]);
  }
  // ...and that list plus `reboot` is the WHOLE table. A command added to
  // cmdauth.h without being considered here fails this.
  TEST_ASSERT_EQUAL_size_t(sizeof(tokenLevel) / sizeof(tokenLevel[0]) + 1, CmdAuth::BUILTIN_COUNT);
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

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_levels_are_ordered_and_named);
  RUN_TEST(test_permits_is_cumulative_not_exact);
  RUN_TEST(test_reboot_is_physical_only);
  RUN_TEST(test_every_other_builtin_is_token);
  RUN_TEST(test_nothing_is_reachable_at_auth_none);
  RUN_TEST(test_unlisted_commands_fail_closed);
  RUN_TEST(test_lookup_is_exact_not_prefix);
  RUN_TEST(test_table_is_well_formed);
  RUN_TEST(test_decision_table_command_by_level);
  RUN_TEST(test_deny_message_names_the_command_and_both_levels);
  RUN_TEST(test_deny_message_fits_a_cmderror_buffer_for_every_command);
  RUN_TEST(test_deny_message_survives_degenerate_input);
  RUN_TEST(test_deny_message_fits_a_tiny_buffer);
  return UNITY_END();
}
