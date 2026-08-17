// Host-side unit tests for the MODULE/ACTION auth policy (modauth.h).
//
// `pio test -e native` — no hardware, no transports, no registry. The sibling
// of test_cmdauth.cpp, and it exists for the same reason: the failure mode is
// silent. An action that quietly sits at AUTH_NONE, a module whose minAuth was
// never written, or a gate ordered so that ENOMOD leaks before EAUTH, all
// produce a device that works perfectly and answers a stranger.
//
// What makes these tests worth anything is that registry.cpp CALLS
// ModAuth::decide() rather than reimplementing it, and every ModuleAction /
// ModuleDescriptor in the image initialises its minAuth from
// ModAuth::requiredFor() / ModAuth::moduleMinimum() at compile time. The native
// env sets build_src_filter = -<*>, so registry.cpp and the mod_*.cpp files are
// not built here at all — a policy restated in them would be untestable, which
// is precisely why it is not restated in them.
//
// Backlog S6 (module actions had no auth check), S7 (dispatch answered before
// the gate) and S8 (the `led` alias and the `led` module disagreed).

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modauth.h"

void setUp() {}
void tearDown() {}

// The three caller levels, in one array, so every table below is walked at all
// of them rather than at the one the author had in mind.
static const uint8_t LEVELS[] = {ModAuth::NONE, ModAuth::TOKEN, ModAuth::PHYSICAL};

// ---- levels and the fail-closed default ---------------------------------

void test_levels_match_cmdauth() {
  // One set of constants for both policies is what makes an identical refusal
  // cheap. If these ever diverge, a module's EAUTH and a built-in's stop being
  // the same thing.
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::NONE, ModAuth::NONE);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::TOKEN, ModAuth::TOKEN);
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::PHYSICAL, ModAuth::PHYSICAL);
  TEST_ASSERT_TRUE(ModAuth::NONE < ModAuth::TOKEN);
  TEST_ASSERT_TRUE(ModAuth::TOKEN < ModAuth::PHYSICAL);
}

void test_an_undeclared_level_fails_closed() {
  // 0 is what an aggregate initialiser leaves behind when the field was
  // forgotten, and it is also the encoding of AUTH_NONE. Both must come back
  // PHYSICAL, or a forgotten field is an open door.
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::effective(0));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::effective(ModAuth::NONE));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::TOKEN, ModAuth::effective(ModAuth::TOKEN));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::effective(ModAuth::PHYSICAL));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::UNLISTED);
}

void test_unlisted_modules_and_actions_fail_closed() {
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::moduleMinimum("msc"));       // F2, not written yet
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::moduleMinimum("wifiscan"));  // F3
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::moduleMinimum(""));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::requiredFor("led", "nosuch"));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::requiredFor("nosuch", "set"));
  TEST_ASSERT_FALSE(ModAuth::isModuleListed("msc"));
  TEST_ASSERT_FALSE(ModAuth::isListed("hid", "mouse"));
}

void test_lookup_is_exact_not_prefix() {
  // "displays" must not match "display", and an action is keyed by BOTH names —
  // otherwise `storage.format` would answer for a future `led.format`.
  TEST_ASSERT_FALSE(ModAuth::isModuleListed("displays"));
  TEST_ASSERT_FALSE(ModAuth::isModuleListed("disp"));
  TEST_ASSERT_FALSE(ModAuth::isListed("led", "se"));
  TEST_ASSERT_FALSE(ModAuth::isListed("led", "format"));
  TEST_ASSERT_TRUE(ModAuth::isListed("storage", "format"));
}

// ---- the shipped table ---------------------------------------------------

void test_nothing_is_reachable_at_auth_none() {
  // THE hard requirement. Asserted as a static_assert in modauth.h too; this is
  // the runtime restatement, because a weakened assert is easier to miss than a
  // failing test.
  for (size_t i = 0; i < ModAuth::MODULE_COUNT; i++) {
    TEST_ASSERT_TRUE_MESSAGE(ModAuth::MODULES[i].minAuth >= ModAuth::TOKEN, ModAuth::MODULES[i].mod);
    TEST_ASSERT_FALSE(ModAuth::moduleVisible(ModAuth::NONE, ModAuth::MODULES[i].minAuth));
  }
  for (size_t i = 0; i < ModAuth::ACTION_COUNT; i++) {
    TEST_ASSERT_TRUE_MESSAGE(ModAuth::ACTIONS[i].minAuth >= ModAuth::TOKEN, ModAuth::ACTIONS[i].act);
    TEST_ASSERT_FALSE(ModAuth::actionAllowed(ModAuth::NONE, ModAuth::moduleMinimum(ModAuth::ACTIONS[i].mod),
                                             ModAuth::ACTIONS[i].minAuth));
  }
  TEST_ASSERT_TRUE(ModAuth::lowestModuleMinimum() >= ModAuth::TOKEN);
}

void test_the_three_physical_actions_are_exactly_these() {
  // "Do not weaken anything currently at AUTH_PHYSICAL", written as a whitelist
  // rather than as three spot checks: this fails if one is lowered AND if a
  // fourth is added without a decision.
  for (size_t i = 0; i < ModAuth::ACTION_COUNT; i++) {
    bool expectPhysical = (strcmp(ModAuth::ACTIONS[i].mod, "storage") == 0 && strcmp(ModAuth::ACTIONS[i].act, "format") == 0) ||
                          (strcmp(ModAuth::ACTIONS[i].mod, "http") == 0 && strcmp(ModAuth::ACTIONS[i].act, "psk") == 0) ||
                          (strcmp(ModAuth::ACTIONS[i].mod, "http") == 0 && strcmp(ModAuth::ACTIONS[i].act, "pin") == 0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expectPhysical ? ModAuth::PHYSICAL : ModAuth::TOKEN, ModAuth::ACTIONS[i].minAuth,
                                    ModAuth::ACTIONS[i].act);
  }
}

void test_the_seven_ungated_actions_from_s6_are_now_gated() {
  // The exact list backlog S6 names. Each was reachable at AUTH_NONE because
  // its module chose so; none is now.
  static const char *const MOD[] = {"led", "led", "hid", "storage", "display", "display", "display"};
  static const char *const ACT[] = {"set", "auto", "status", "status", "status", "screen", "refresh"};
  for (size_t i = 0; i < sizeof(MOD) / sizeof(MOD[0]); i++) {
    TEST_ASSERT_TRUE_MESSAGE(ModAuth::isListed(MOD[i], ACT[i]), ACT[i]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ModAuth::TOKEN, ModAuth::requiredFor(MOD[i], ACT[i]), ACT[i]);
    TEST_ASSERT_FALSE(ModAuth::actionAllowed(ModAuth::NONE, ModAuth::moduleMinimum(MOD[i]), ModAuth::requiredFor(MOD[i], ACT[i])));
  }
  // hid.release was the eighth: not in S6's list, but it was explicitly
  // allowed at ANY level in the module as a "panic stop".
  TEST_ASSERT_EQUAL_UINT8(ModAuth::TOKEN, ModAuth::requiredFor("hid", "release"));
}

void test_s8_the_led_alias_and_the_led_action_agree() {
  // The `led` built-in is a pure alias for {"mod":"led","act":"set"}. Two bars
  // for one capability is a way round whichever is higher. console.cpp carries
  // the same comparison as a static_assert; this is its runtime twin.
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::requiredFor("led"), ModAuth::requiredFor("led", "set"));
  TEST_ASSERT_EQUAL_UINT8(CmdAuth::TOKEN, ModAuth::requiredFor("led", "set"));
}

void test_every_action_names_a_listed_module_and_sits_at_or_above_it() {
  for (size_t i = 0; i < ModAuth::ACTION_COUNT; i++) {
    TEST_ASSERT_TRUE_MESSAGE(ModAuth::isModuleListed(ModAuth::ACTIONS[i].mod), ModAuth::ACTIONS[i].mod);
    TEST_ASSERT_TRUE_MESSAGE(ModAuth::ACTIONS[i].minAuth >= ModAuth::moduleMinimum(ModAuth::ACTIONS[i].mod),
                             ModAuth::ACTIONS[i].act);
  }
}

void test_no_duplicate_rows() {
  for (size_t i = 0; i < ModAuth::MODULE_COUNT; i++) {
    for (size_t j = i + 1; j < ModAuth::MODULE_COUNT; j++) {
      TEST_ASSERT_FALSE(strcmp(ModAuth::MODULES[i].mod, ModAuth::MODULES[j].mod) == 0);
    }
  }
  for (size_t i = 0; i < ModAuth::ACTION_COUNT; i++) {
    for (size_t j = i + 1; j < ModAuth::ACTION_COUNT; j++) {
      bool same = strcmp(ModAuth::ACTIONS[i].mod, ModAuth::ACTIONS[j].mod) == 0 &&
                  strcmp(ModAuth::ACTIONS[i].act, ModAuth::ACTIONS[j].act) == 0;
      TEST_ASSERT_FALSE(same);
    }
  }
}

// ---- the decision table: module x action x caller level ------------------

// What registry.cpp will do, for the whole shipped surface at every level.
void test_decision_table_every_action_at_every_level() {
  const uint8_t lowest = ModAuth::lowestModuleMinimum();
  for (size_t l = 0; l < sizeof(LEVELS) / sizeof(LEVELS[0]); l++) {
    uint8_t have = LEVELS[l];
    for (size_t i = 0; i < ModAuth::ACTION_COUNT; i++) {
      const ModAuth::ActionPolicy &a = ModAuth::ACTIONS[i];
      uint8_t modMin = ModAuth::moduleMinimum(a.mod);
      ModAuth::Decision got = ModAuth::decide(have, lowest, true, modMin, a.minAuth);

      if (have < ModAuth::TOKEN) {
        // Below every module's bar: one answer for everything, and it is the
        // one that names nothing.
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ModAuth::DECIDE_EAUTH_ANY, got, a.act);
      } else if (have < a.minAuth) {
        // The module is reachable, the action is not — the only case that
        // distinguishes an action's bar from its module's.
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ModAuth::DECIDE_EAUTH_ACTION, got, a.act);
      } else {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ModAuth::DECIDE_ALLOW, got, a.act);
      }
      // And the boolean a UI greys out with must agree with the decision.
      TEST_ASSERT_EQUAL_MESSAGE(got == ModAuth::DECIDE_ALLOW, ModAuth::actionAllowed(have, modMin, a.minAuth), a.act);
    }
  }
}

void test_a_token_session_reaches_everything_except_the_three_secrets() {
  const uint8_t lowest = ModAuth::lowestModuleMinimum();
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_ALLOW, ModAuth::decide(ModAuth::TOKEN, lowest, true,
                                                                 ModAuth::moduleMinimum("hid"),
                                                                 ModAuth::requiredFor("hid", "type")));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_EAUTH_ACTION, ModAuth::decide(ModAuth::TOKEN, lowest, true,
                                                                        ModAuth::moduleMinimum("http"),
                                                                        ModAuth::requiredFor("http", "psk")));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_EAUTH_ACTION, ModAuth::decide(ModAuth::TOKEN, lowest, true,
                                                                        ModAuth::moduleMinimum("http"),
                                                                        ModAuth::requiredFor("http", "pin")));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_EAUTH_ACTION, ModAuth::decide(ModAuth::TOKEN, lowest, true,
                                                                        ModAuth::moduleMinimum("storage"),
                                                                        ModAuth::requiredFor("storage", "format")));
  // The cable clears all three.
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_ALLOW, ModAuth::decide(ModAuth::PHYSICAL, lowest, true,
                                                                 ModAuth::moduleMinimum("storage"),
                                                                 ModAuth::requiredFor("storage", "format")));
}

// ---- S7: ENOMOD must be indistinguishable from EAUTH below the bar -------

void test_below_the_bar_enomod_and_eauth_are_the_same_answer() {
  const uint8_t lowest = ModAuth::lowestModuleMinimum();
  // A real module, a real action.
  ModAuth::Decision real = ModAuth::decide(ModAuth::NONE, lowest, true, ModAuth::moduleMinimum("hid"),
                                           ModAuth::requiredFor("hid", "status"));
  // A module that does not exist. Same caller, same call, same answer — so the
  // difference between "no such module" and "not allowed" carries no
  // information, and the module map cannot be enumerated.
  ModAuth::Decision fake = ModAuth::decide(ModAuth::NONE, lowest, false, ModAuth::moduleMinimum("nosuchmodule"),
                                           ModAuth::requiredFor("nosuchmodule", "anything"));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_EAUTH_ANY, real);
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_EAUTH_ANY, fake);
  TEST_ASSERT_EQUAL_UINT8(real, fake);

  // Exhaustively: at AUTH_NONE, EVERY module in the image and every invented
  // name give the identical answer.
  for (size_t i = 0; i < ModAuth::MODULE_COUNT; i++) {
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        ModAuth::DECIDE_EAUTH_ANY,
        ModAuth::decide(ModAuth::NONE, lowest, true, ModAuth::MODULES[i].minAuth, ModAuth::TOKEN),
        ModAuth::MODULES[i].mod);
  }
}

void test_above_the_bar_enomod_is_honest() {
  // Once the caller holds a session it is told the truth about a name that does
  // not exist — the same trade S1 makes when it answers EUNKNOWN to an
  // authenticated caller of a built-in.
  const uint8_t lowest = ModAuth::lowestModuleMinimum();
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_ENOMOD,
                          ModAuth::decide(ModAuth::TOKEN, lowest, false, ModAuth::PHYSICAL, ModAuth::PHYSICAL));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_ENOMOD,
                          ModAuth::decide(ModAuth::PHYSICAL, lowest, false, ModAuth::PHYSICAL, ModAuth::PHYSICAL));
}

void test_the_module_bar_is_answered_before_the_action_bar() {
  // A hypothetical PHYSICAL-only module: a TOKEN caller must be refused by the
  // MODULE, not told which of its actions it could have run. That ordering is
  // what keeps EDISABLED/EREBOOT — and the action table — behind the gate.
  const uint8_t lowest = ModAuth::TOKEN;  // some other module is reachable at TOKEN
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_EAUTH_MODULE,
                          ModAuth::decide(ModAuth::TOKEN, lowest, true, ModAuth::PHYSICAL, ModAuth::TOKEN));
  TEST_ASSERT_EQUAL_UINT8(ModAuth::DECIDE_ALLOW,
                          ModAuth::decide(ModAuth::PHYSICAL, lowest, true, ModAuth::PHYSICAL, ModAuth::TOKEN));
}

// ---- visibility, i.e. what Registry::list() renders ----------------------

void test_list_visibility_by_level() {
  // AUTH_NONE sees NOTHING — not a stub, not a name.
  uint8_t visibleAtNone = 0, visibleAtToken = 0, visibleAtPhysical = 0;
  for (size_t i = 0; i < ModAuth::MODULE_COUNT; i++) {
    if (ModAuth::moduleVisible(ModAuth::NONE, ModAuth::MODULES[i].minAuth)) visibleAtNone++;
    if (ModAuth::moduleVisible(ModAuth::TOKEN, ModAuth::MODULES[i].minAuth)) visibleAtToken++;
    if (ModAuth::moduleVisible(ModAuth::PHYSICAL, ModAuth::MODULES[i].minAuth)) visibleAtPhysical++;
  }
  TEST_ASSERT_EQUAL_UINT8(0, visibleAtNone);
  TEST_ASSERT_EQUAL_UINT8(ModAuth::MODULE_COUNT, visibleAtToken);
  TEST_ASSERT_EQUAL_UINT8(ModAuth::MODULE_COUNT, visibleAtPhysical);
}

void test_a_visible_module_can_still_hold_actions_the_caller_cannot_run() {
  // The reason `allowed` is emitted per action rather than per module: `http`
  // and `storage` are fully visible to a TOKEN session, and three of their
  // actions are not runnable by it.
  TEST_ASSERT_TRUE(ModAuth::moduleVisible(ModAuth::TOKEN, ModAuth::moduleMinimum("http")));
  TEST_ASSERT_FALSE(ModAuth::actionAllowed(ModAuth::TOKEN, ModAuth::moduleMinimum("http"), ModAuth::requiredFor("http", "psk")));
  TEST_ASSERT_TRUE(ModAuth::actionAllowed(ModAuth::TOKEN, ModAuth::moduleMinimum("http"), ModAuth::requiredFor("http", "status")));
}

// ---- the descriptor-side guard rails -------------------------------------

void test_allGated_catches_a_forgotten_field() {
  // Exactly the shape a mod_*.cpp table has, including the one a careless edit
  // produces: three fields written, the level left off.
  static const ModuleAction good[] = {
      {"a", "help", nullptr, 0, ModAuth::TOKEN},
      {"b", "help", nullptr, 0, ModAuth::PHYSICAL},
  };
  // The pragma is the point being made, not a wart: under -Wextra the host
  // build catches this omission by itself, which is a THIRD net after
  // allGated() and effective(). The device envs do not build with -Wextra, so
  // the other two still have to exist — and this fixture has to opt out of the
  // warning to be able to test them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  static const ModuleAction forgotten[] = {
      {"a", "help", nullptr, 0, ModAuth::TOKEN},
      {"b", "help", nullptr, 0},  // <- the bug this exists to catch
  };
#pragma GCC diagnostic pop
  TEST_ASSERT_TRUE(ModAuth::allGated(good, 2));
  TEST_ASSERT_FALSE(ModAuth::allGated(forgotten, 2));
  // And it must be caught by the value, not by luck: the forgotten field is 0,
  // which effective() turns into PHYSICAL at runtime as the second net.
  TEST_ASSERT_EQUAL_UINT8(0, forgotten[1].minAuth);
  TEST_ASSERT_EQUAL_UINT8(ModAuth::PHYSICAL, ModAuth::effective(forgotten[1].minAuth));
}

void test_a_module_with_no_declared_minimum_is_invisible_to_the_network() {
  // A descriptor whose minAuth was never initialised: nothing on the network
  // may see it, and only the cable may use it.
  TEST_ASSERT_FALSE(ModAuth::moduleVisible(ModAuth::NONE, 0));
  TEST_ASSERT_FALSE(ModAuth::moduleVisible(ModAuth::TOKEN, 0));
  TEST_ASSERT_TRUE(ModAuth::moduleVisible(ModAuth::PHYSICAL, 0));
}

// ---- the refusal ---------------------------------------------------------

void test_the_refusal_is_the_builtins_refusal() {
  // Same function, same wording, same truncation point as console.cpp's
  // authDenied() — a caller must not be able to tell a module's EAUTH from a
  // built-in's, or the shape of the refusal maps the device.
  char mod[96];
  char builtin[96];
  CmdAuth::denyMessage(mod, sizeof(mod), "the module 'hid'", ModAuth::TOKEN, "ble", ModAuth::NONE);
  CmdAuth::denyMessage(builtin, sizeof(builtin), "the built-in command 'info'", CmdAuth::TOKEN, "ble", CmdAuth::NONE);
  TEST_ASSERT_EQUAL_STRING("the module 'hid' needs auth >= token; transport 'ble' is at level 0", mod);
  TEST_ASSERT_NOT_NULL(strstr(builtin, "needs auth >= token; transport 'ble' is at level 0"));
}

void test_every_refusal_fits_a_cmderror_message() {
  // CmdError::msg is 96 bytes. A message that truncated would be recognisable
  // BY ITS LENGTH, which is a side channel of exactly the kind this policy
  // exists to close. Longest phrasings registry.cpp can build:
  //   "the module '<=16 chars>'" and "the action '<=16>.<=24>'".
  char msg[96];
  for (size_t i = 0; i < ModAuth::ACTION_COUNT; i++) {
    char what[64];
    snprintf(what, sizeof(what), "the action '%.16s.%.24s'", ModAuth::ACTIONS[i].mod, ModAuth::ACTIONS[i].act);
    size_t n = CmdAuth::denyMessage(msg, sizeof(msg), what, ModAuth::ACTIONS[i].minAuth, "http", ModAuth::TOKEN);
    TEST_ASSERT_TRUE_MESSAGE(n < sizeof(msg) - 1, ModAuth::ACTIONS[i].act);
  }
  for (size_t i = 0; i < ModAuth::MODULE_COUNT; i++) {
    char what[48];
    snprintf(what, sizeof(what), "the module '%.16s'", ModAuth::MODULES[i].mod);
    size_t n = CmdAuth::denyMessage(msg, sizeof(msg), what, ModAuth::MODULES[i].minAuth, "http", ModAuth::NONE);
    TEST_ASSERT_TRUE_MESSAGE(n < sizeof(msg) - 1, ModAuth::MODULES[i].mod);
  }
  size_t n = CmdAuth::denyMessage(msg, sizeof(msg), "any module", ModAuth::TOKEN, "ble", ModAuth::NONE);
  TEST_ASSERT_TRUE(n < sizeof(msg) - 1);
  TEST_ASSERT_EQUAL_STRING("any module needs auth >= token; transport 'ble' is at level 0", msg);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_levels_match_cmdauth);
  RUN_TEST(test_an_undeclared_level_fails_closed);
  RUN_TEST(test_unlisted_modules_and_actions_fail_closed);
  RUN_TEST(test_lookup_is_exact_not_prefix);
  RUN_TEST(test_nothing_is_reachable_at_auth_none);
  RUN_TEST(test_the_three_physical_actions_are_exactly_these);
  RUN_TEST(test_the_seven_ungated_actions_from_s6_are_now_gated);
  RUN_TEST(test_s8_the_led_alias_and_the_led_action_agree);
  RUN_TEST(test_every_action_names_a_listed_module_and_sits_at_or_above_it);
  RUN_TEST(test_no_duplicate_rows);
  RUN_TEST(test_decision_table_every_action_at_every_level);
  RUN_TEST(test_a_token_session_reaches_everything_except_the_three_secrets);
  RUN_TEST(test_below_the_bar_enomod_and_eauth_are_the_same_answer);
  RUN_TEST(test_above_the_bar_enomod_is_honest);
  RUN_TEST(test_the_module_bar_is_answered_before_the_action_bar);
  RUN_TEST(test_list_visibility_by_level);
  RUN_TEST(test_a_visible_module_can_still_hold_actions_the_caller_cannot_run);
  RUN_TEST(test_allGated_catches_a_forgotten_field);
  RUN_TEST(test_a_module_with_no_declared_minimum_is_invisible_to_the_network);
  RUN_TEST(test_the_refusal_is_the_builtins_refusal);
  RUN_TEST(test_every_refusal_fits_a_cmderror_message);
  return UNITY_END();
}
