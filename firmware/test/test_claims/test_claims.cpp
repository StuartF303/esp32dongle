// Host-side unit tests for the claim arbitration rule (claims.h).
//
// `pio test -e native` — no hardware involved. This is the same assertion
// table the on-target `selftest` console command runs, plus a few properties
// that are cheap to state here and awkward to state on the wire.
//
// The point of this file is not really the assertions; it is that claims.h and
// claims_selftest.h COMPILE FOR THE HOST AT ALL. Both are dependency-free on
// purpose (<stdint.h> and nothing else) so the arbitration logic can be tested
// without a device, and until now nothing enforced that. An accidental
// #include <Arduino.h> in claims.h now fails this env instead of quietly
// removing the property.

#include <unity.h>

#include "claims_selftest.h"

using namespace Claims;

void setUp() {}
void tearDown() {}

// The shared table, asserted the same way the device asserts it.
void test_selftest_table_passes() {
  SelfTestResult res = runSelfTest();
  TEST_ASSERT_TRUE_MESSAGE(res.total > 0, "self-test table is empty");
  if (res.failed > 0 && res.namedFailures > 0) {
    TEST_FAIL_MESSAGE(res.failures[0]);
  }
  TEST_ASSERT_EQUAL_UINT16(0, res.failed);
  TEST_ASSERT_EQUAL_UINT16(res.total, res.passed);
}

// Arbitration must not depend on who asked first.
void test_arbitration_is_symmetric() {
  uint16_t count = 0;
  const SelfTestCase *cases = selfTestCases(&count);
  for (uint16_t i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_MESSAGE(coexist(cases[i].a, cases[i].b), coexist(cases[i].b, cases[i].a), cases[i].name);
  }
}

// A module never conflicts with itself: enable() skips its own index, but the
// rule underneath has to be sane regardless.
void test_every_claimset_coexists_with_none() {
  uint16_t count = 0;
  const SelfTestCase *cases = selfTestCases(&count);
  for (uint16_t i = 0; i < count; i++) {
    TEST_ASSERT_TRUE_MESSAGE(coexist(cases[i].a, none()), cases[i].name);
    TEST_ASSERT_TRUE_MESSAGE(coexist(cases[i].b, none()), cases[i].name);
  }
}

// The reason RES_RADIO was split. An exclusive Wi-Fi claim (monitor mode) has
// no business evicting a BLE scan; under the old single radio resource it did.
void test_wifi_exclusive_does_not_evict_ble() {
  TEST_ASSERT_TRUE(coexist(claim(RES_WIFI, CLAIM_EXCLUSIVE), claim(RES_BLE, CLAIM_SHARED)));
  TEST_ASSERT_TRUE(coexist(claim(RES_WIFI, CLAIM_EXCLUSIVE), claim(RES_BLE, CLAIM_EXCLUSIVE)));
  TEST_ASSERT_FALSE(coexist(claim(RES_WIFI, CLAIM_EXCLUSIVE), claim(RES_WIFI, CLAIM_SHARED)));
  TEST_ASSERT_EQUAL_UINT8(RES_WIFI, firstConflict(claim(RES_WIFI, CLAIM_EXCLUSIVE), claim(RES_WIFI, CLAIM_SHARED)));
}

// firstConflict() reports the LOWEST-indexed conflicting resource, and
// RES_COUNT when there is none. The registry's error messages depend on this.
void test_first_conflict_reports_a_resource_or_res_count() {
  ClaimSet msc = claim(RES_USB, CLAIM_SHARED, RES_SD, CLAIM_EXCLUSIVE);
  ClaimSet storage = claim(RES_SD, CLAIM_SHARED);
  TEST_ASSERT_EQUAL_UINT8(RES_SD, firstConflict(msc, storage));
  TEST_ASSERT_EQUAL_UINT8(RES_COUNT, firstConflict(msc, claim(RES_LED, CLAIM_EXCLUSIVE)));
  TEST_ASSERT_EQUAL_UINT8(RES_COUNT, firstConflict(none(), none()));
}

// The builders exist so descriptors never depend on the enum's ORDER. If they
// stop honouring that, inserting a resource silently reassigns every claim.
void test_builders_set_only_what_was_asked_for() {
  ClaimSet c = claim(RES_SD, CLAIM_EXCLUSIVE, RES_LCD, CLAIM_SHARED);
  for (uint8_t r = 0; r < RES_COUNT; r++) {
    if (r == RES_SD) {
      TEST_ASSERT_EQUAL_UINT8(CLAIM_EXCLUSIVE, c.mode[r]);
    } else if (r == RES_LCD) {
      TEST_ASSERT_EQUAL_UINT8(CLAIM_SHARED, c.mode[r]);
    } else {
      TEST_ASSERT_EQUAL_UINT8(CLAIM_NONE, c.mode[r]);
    }
  }
}

// resourceName() is wire-visible and rendered by the web UI. A resource with
// no name reaches the UI as "?", which is why claims.h carries a static_assert
// on RES_COUNT — this catches the other half: a name that was never added.
void test_every_resource_has_a_name() {
  for (uint8_t r = 0; r < RES_COUNT; r++) {
    const char *n = resourceName(r);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_TRUE_MESSAGE(n[0] != '?' && n[0] != '\0', "a Resource is missing from resourceName()");
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_selftest_table_passes);
  RUN_TEST(test_arbitration_is_symmetric);
  RUN_TEST(test_every_claimset_coexists_with_none);
  RUN_TEST(test_wifi_exclusive_does_not_evict_ble);
  RUN_TEST(test_first_conflict_reports_a_resource_or_res_count);
  RUN_TEST(test_builders_set_only_what_was_asked_for);
  RUN_TEST(test_every_resource_has_a_name);
  return UNITY_END();
}
