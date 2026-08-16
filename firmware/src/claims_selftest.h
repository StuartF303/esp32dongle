// usbdongle W1 — assertion table for the pure claim arbitration function.
//
// Dependency-free like claims.h (only <stdint.h>, via claims.h), so this same
// table is what a future `pio test -e native` runs on the host. The `selftest`
// console command runs it on-target.
//
// These cases are synthetic ClaimSets built here — the registry is NOT
// involved and no fake modules are ever registered in it. Testing must not be
// able to perturb live hardware state.

#pragma once

#include "claims.h"

namespace Claims {

struct SelfTestCase {
  const char *name;
  ClaimSet a;
  ClaimSet b;
  bool expectCoexist;
  uint8_t expectConflictOn;  // resource index, or RES_COUNT when they coexist
};

struct SelfTestResult {
  uint16_t total;
  uint16_t passed;
  uint16_t failed;
  static const uint8_t MAX_NAMED_FAILURES = 4;
  const char *failures[MAX_NAMED_FAILURES];
  uint8_t namedFailures;
};

inline const SelfTestCase *selfTestCases(uint16_t *countOut) {
  // `static const` (not constexpr) so this lives in .rodata and costs no RAM.
  static const SelfTestCase CASES[] = {
      {"shared+shared coexist", claim(RES_RADIO, CLAIM_SHARED), claim(RES_RADIO, CLAIM_SHARED), true, RES_COUNT},
      {"shared+exclusive conflict", claim(RES_RADIO, CLAIM_SHARED), claim(RES_RADIO, CLAIM_EXCLUSIVE), false,
       RES_RADIO},
      {"exclusive+exclusive conflict", claim(RES_USB, CLAIM_EXCLUSIVE), claim(RES_USB, CLAIM_EXCLUSIVE), false,
       RES_USB},
      {"disjoint resources coexist", claim(RES_USB, CLAIM_EXCLUSIVE), claim(RES_LCD, CLAIM_EXCLUSIVE), true,
       RES_COUNT},
      {"multi-resource partial conflict",
       claim(RES_USB, CLAIM_EXCLUSIVE, RES_SD, CLAIM_EXCLUSIVE),  // msc
       claim(RES_SD, CLAIM_SHARED),                               // storage
       false, RES_SD},
      {"multi-resource disjoint coexist", claim(RES_USB, CLAIM_EXCLUSIVE, RES_LED, CLAIM_EXCLUSIVE),
       claim(RES_SD, CLAIM_EXCLUSIVE, RES_LCD, CLAIM_EXCLUSIVE), true, RES_COUNT},
      {"multi-resource all shared coexist", claim(RES_RADIO, CLAIM_SHARED, RES_SD, CLAIM_SHARED),
       claim(RES_RADIO, CLAIM_SHARED, RES_SD, CLAIM_SHARED), true, RES_COUNT},
      {"none never conflicts", none(), claim(RES_USB, CLAIM_EXCLUSIVE), true, RES_COUNT},
      {"empty vs empty", none(), none(), true, RES_COUNT},
      // The three real cases the model exists for, spelled out.
      {"wifiscan+blescan coexist", claim(RES_RADIO, CLAIM_SHARED), claim(RES_RADIO, CLAIM_SHARED), true, RES_COUNT},
      {"monitor mode locks out blescan", claim(RES_RADIO, CLAIM_EXCLUSIVE), claim(RES_RADIO, CLAIM_SHARED), false,
       RES_RADIO},
      {"msc locks out hid", claim(RES_USB, CLAIM_EXCLUSIVE, RES_SD, CLAIM_EXCLUSIVE), claim(RES_USB, CLAIM_EXCLUSIVE),
       false, RES_USB},
  };
  *countOut = (uint16_t)(sizeof(CASES) / sizeof(CASES[0]));
  return CASES;
}

// Each case is asserted three ways: coexist(a,b), the resource reported by
// firstConflict(a,b), and coexist(b,a) — arbitration must be symmetric, or
// "who asked first" would change the answer.
inline SelfTestResult runSelfTest() {
  SelfTestResult res{};
  uint16_t count = 0;
  const SelfTestCase *cases = selfTestCases(&count);

  for (uint16_t i = 0; i < count; i++) {
    const SelfTestCase &c = cases[i];
    bool ok = coexist(c.a, c.b) == c.expectCoexist;
    ok = ok && firstConflict(c.a, c.b) == c.expectConflictOn;
    ok = ok && coexist(c.b, c.a) == c.expectCoexist;

    res.total++;
    if (ok) {
      res.passed++;
    } else {
      res.failed++;
      if (res.namedFailures < SelfTestResult::MAX_NAMED_FAILURES) {
        res.failures[res.namedFailures++] = c.name;
      }
    }
  }
  return res;
}

}  // namespace Claims
