// Host-side unit tests for the machine-readable action parameter schema
// (modparam.h).
//
// `pio test -e native` — no hardware. What is worth testing here is the part
// that turns static descriptor data into what a form renders, because a wrong
// answer is invisible on the device and shows up as a control that sends the
// wrong thing:
//
//   * typeName() must never map an unrecognised type to "string" — a UI that
//     renders an unknown future type as a text box mistypes it silently;
//   * the enum splitter must agree with itself, i.e. enumValueAt() must index
//     exactly the values enumCount() counted, including around empty entries;
//   * enumValueAt() must report the value's FULL length so the emitter can
//     tell "copied" from "truncated" instead of shipping a shortened value the
//     module would reject.
//
// The JSON emission itself lives in Registry::list(), which needs ArduinoJson
// and FreeRTOS and is not host-buildable; everything it depends on is here.

#include <unity.h>

#include <string.h>

#include "modparam.h"

void setUp() {}
void tearDown() {}

// ---- type names ---------------------------------------------------------

void test_every_type_has_a_distinct_wire_name() {
  TEST_ASSERT_EQUAL_STRING("string", ModParam::typeName(P_STRING));
  TEST_ASSERT_EQUAL_STRING("int", ModParam::typeName(P_INT));
  TEST_ASSERT_EQUAL_STRING("bool", ModParam::typeName(P_BOOL));
  TEST_ASSERT_EQUAL_STRING("enum", ModParam::typeName(P_ENUM));
  TEST_ASSERT_EQUAL_STRING("enum_list", ModParam::typeName(P_ENUM_LIST));
}

// The failure this prevents: a sixth ParamType added without a name, silently
// rendered by the web UI as a text field that mistypes every value it sends.
void test_an_unknown_type_is_not_silently_a_string() {
  TEST_ASSERT_EQUAL_STRING("unknown", ModParam::typeName(200));
  TEST_ASSERT_EQUAL_STRING("unknown", ModParam::typeName(P_ENUM_LIST + 1));
}

// ---- enum splitting -----------------------------------------------------

void test_enum_count() {
  TEST_ASSERT_EQUAL_UINT8(0, ModParam::enumCount(nullptr));
  TEST_ASSERT_EQUAL_UINT8(0, ModParam::enumCount(""));
  TEST_ASSERT_EQUAL_UINT8(1, ModParam::enumCount("status"));
  TEST_ASSERT_EQUAL_UINT8(2, ModParam::enumCount("status,diag"));
  TEST_ASSERT_EQUAL_UINT8(4, ModParam::enumCount("ctrl,shift,alt,gui"));
}

// Empty entries are skipped rather than counted, so a stray comma cannot make
// the count and the index disagree — which would emit an empty string as a
// selectable enum value.
void test_enum_count_skips_empty_entries() {
  TEST_ASSERT_EQUAL_UINT8(2, ModParam::enumCount("a,,b"));
  TEST_ASSERT_EQUAL_UINT8(2, ModParam::enumCount(",a,b,"));
  TEST_ASSERT_EQUAL_UINT8(0, ModParam::enumCount(",,,"));
}

void test_enum_value_at_returns_each_value_in_order() {
  char v[16];
  TEST_ASSERT_EQUAL_UINT32(6, ModParam::enumValueAt("status,diag", 0, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("status", v);
  TEST_ASSERT_EQUAL_UINT32(4, ModParam::enumValueAt("status,diag", 1, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("diag", v);
}

void test_enum_value_at_indexes_the_same_set_enum_count_counted() {
  const char *csv = ",en_GB,,en_US,";
  char v[16];
  TEST_ASSERT_EQUAL_UINT8(2, ModParam::enumCount(csv));
  TEST_ASSERT_EQUAL_UINT32(5, ModParam::enumValueAt(csv, 0, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("en_GB", v);
  TEST_ASSERT_EQUAL_UINT32(5, ModParam::enumValueAt(csv, 1, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("en_US", v);
}

void test_enum_value_at_past_the_end_is_empty_not_stale() {
  char v[16];
  strcpy(v, "leftover");
  TEST_ASSERT_EQUAL_UINT32(0, ModParam::enumValueAt("a,b", 2, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("", v);
  TEST_ASSERT_EQUAL_UINT32(0, ModParam::enumValueAt(nullptr, 0, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("", v);
}

// THE TRUNCATION CONTRACT. The return is the value's length, not the number of
// bytes copied, so the caller can tell it happened. Registry::list() uses
// exactly this to emit "enum_truncated": true rather than a shortened value.
void test_enum_value_at_reports_truncation_and_never_overruns() {
  char v[4];
  memset(v, 'x', sizeof(v));
  size_t n = ModParam::enumValueAt("abcdefgh,z", 0, v, sizeof(v));
  TEST_ASSERT_EQUAL_UINT32(8, n);
  TEST_ASSERT_TRUE(n >= sizeof(v));  // how the emitter detects it
  TEST_ASSERT_EQUAL_STRING("abc", v);
  // The value AFTER a truncated one is still found: truncation is a copy
  // problem, not a parse problem.
  TEST_ASSERT_EQUAL_UINT32(1, ModParam::enumValueAt("abcdefgh,z", 1, v, sizeof(v)));
  TEST_ASSERT_EQUAL_STRING("z", v);
}

// Every enum value in the tree must fit the emitter's buffer, or the wire
// carries "enum_truncated" and a UI offers a value the module will reject.
void test_the_shipped_enum_values_all_fit() {
  const char *shipped[] = {"ctrl,shift,alt,gui", "en_GB,en_US", "status,diag"};
  char v[ModParam::MAX_ENUM_VALUE + 1];
  for (const char *csv : shipped) {
    uint8_t n = ModParam::enumCount(csv);
    TEST_ASSERT_TRUE(n > 0);
    for (uint8_t i = 0; i < n; i++) {
      TEST_ASSERT_TRUE_MESSAGE(ModParam::enumValueAt(csv, i, v, sizeof(v)) <= ModParam::MAX_ENUM_VALUE,
                               "an enum value is longer than the emitter's buffer");
    }
  }
}

// ---- the constructors ---------------------------------------------------

void test_constructors_fill_exactly_what_they_claim() {
  constexpr ModuleParam s = ModParam::str("rgb", true, "six hex digits");
  TEST_ASSERT_EQUAL_STRING("rgb", s.name);
  TEST_ASSERT_EQUAL_UINT8(P_STRING, s.type);
  TEST_ASSERT_TRUE(s.required);
  TEST_ASSERT_NULL(s.enumVals);

  constexpr ModuleParam n = ModParam::num("wpm", false, "speed", 1, 2000);
  TEST_ASSERT_EQUAL_UINT8(P_INT, n.type);
  TEST_ASSERT_FALSE(n.required);
  TEST_ASSERT_EQUAL_INT32(1, n.min);
  TEST_ASSERT_EQUAL_INT32(2000, n.max);

  // An unbounded int must carry the sentinels, which Registry::list() omits —
  // emitting INT32_MIN as a "min" would put a spinner bound nobody meant into
  // the UI.
  constexpr ModuleParam u = ModParam::num("any", false, "unbounded");
  TEST_ASSERT_EQUAL_INT32(ModParam::NO_MIN, u.min);
  TEST_ASSERT_EQUAL_INT32(ModParam::NO_MAX, u.max);

  constexpr ModuleParam f = ModParam::flag("cancel", false, "stop it");
  TEST_ASSERT_EQUAL_UINT8(P_BOOL, f.type);

  constexpr ModuleParam c = ModParam::choice("name", true, "which", "status,diag");
  TEST_ASSERT_EQUAL_UINT8(P_ENUM, c.type);
  TEST_ASSERT_EQUAL_STRING("status,diag", c.enumVals);

  constexpr ModuleParam l = ModParam::choiceList("mods", false, "held", "ctrl,gui");
  TEST_ASSERT_EQUAL_UINT8(P_ENUM_LIST, l.type);
  TEST_ASSERT_EQUAL_STRING("ctrl,gui", l.enumVals);
}

// MOD_PARAMS() exists so a hand-written count cannot drift from its table and
// walk off the end of .rodata.
void test_mod_params_macro_counts_its_own_table() {
  static const ModuleParam table[] = {
      ModParam::str("a", true, ""),
      ModParam::flag("b", false, ""),
      ModParam::num("c", false, "", 0, 9),
  };
  struct Holder {
    const ModuleParam *params;
    uint8_t paramCount;
  };
  Holder h = {MOD_PARAMS(table)};
  TEST_ASSERT_EQUAL_UINT8(3, h.paramCount);
  TEST_ASSERT_EQUAL_STRING("c", h.params[h.paramCount - 1].name);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_type_has_a_distinct_wire_name);
  RUN_TEST(test_an_unknown_type_is_not_silently_a_string);
  RUN_TEST(test_enum_count);
  RUN_TEST(test_enum_count_skips_empty_entries);
  RUN_TEST(test_enum_value_at_returns_each_value_in_order);
  RUN_TEST(test_enum_value_at_indexes_the_same_set_enum_count_counted);
  RUN_TEST(test_enum_value_at_past_the_end_is_empty_not_stale);
  RUN_TEST(test_enum_value_at_reports_truncation_and_never_overruns);
  RUN_TEST(test_the_shipped_enum_values_all_fit);
  RUN_TEST(test_constructors_fill_exactly_what_they_claim);
  RUN_TEST(test_mod_params_macro_counts_its_own_table);
  return UNITY_END();
}
