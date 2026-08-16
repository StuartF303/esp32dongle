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
      {"shared+shared coexist", claim(RES_WIFI, CLAIM_SHARED), claim(RES_WIFI, CLAIM_SHARED), true, RES_COUNT},
      {"shared+exclusive conflict", claim(RES_WIFI, CLAIM_SHARED), claim(RES_WIFI, CLAIM_EXCLUSIVE), false, RES_WIFI},
      {"exclusive+exclusive conflict", claim(RES_USB, CLAIM_EXCLUSIVE), claim(RES_USB, CLAIM_EXCLUSIVE), false,
       RES_USB},
      {"disjoint resources coexist", claim(RES_USB, CLAIM_EXCLUSIVE), claim(RES_LCD, CLAIM_EXCLUSIVE), true,
       RES_COUNT},
      {"multi-resource partial conflict",
       claim(RES_USB, CLAIM_SHARED, RES_SD, CLAIM_EXCLUSIVE),  // msc
       claim(RES_SD, CLAIM_SHARED),                            // storage
       false, RES_SD},
      {"multi-resource disjoint coexist", claim(RES_USB, CLAIM_EXCLUSIVE, RES_LED, CLAIM_EXCLUSIVE),
       claim(RES_SD, CLAIM_EXCLUSIVE, RES_LCD, CLAIM_EXCLUSIVE), true, RES_COUNT},
      {"multi-resource all shared coexist", claim(RES_WIFI, CLAIM_SHARED, RES_SD, CLAIM_SHARED),
       claim(RES_WIFI, CLAIM_SHARED, RES_SD, CLAIM_SHARED), true, RES_COUNT},
      {"none never conflicts", none(), claim(RES_USB, CLAIM_EXCLUSIVE), true, RES_COUNT},
      {"empty vs empty", none(), none(), true, RES_COUNT},

      // The real cases the model exists for, spelled out.
      {"wifiscan+blescan coexist", claim(RES_WIFI, CLAIM_SHARED), claim(RES_BLE, CLAIM_SHARED), true, RES_COUNT},
      {"monitor mode locks out wifiscan", claim(RES_WIFI, CLAIM_EXCLUSIVE), claim(RES_WIFI, CLAIM_SHARED), false,
       RES_WIFI},
      // The whole point of splitting RES_RADIO: an exclusive Wi-Fi claim must
      // NOT evict BLE. Under the old single RES_RADIO this case asserted the
      // opposite, and it was wrong.
      {"monitor mode does NOT evict blescan", claim(RES_WIFI, CLAIM_EXCLUSIVE), claim(RES_BLE, CLAIM_SHARED), true,
       RES_COUNT},
      {"ble advertise locks out blescan", claim(RES_BLE, CLAIM_EXCLUSIVE), claim(RES_BLE, CLAIM_SHARED), false,
       RES_BLE},
      // Corrected USB model (2026-08-16): on this framework the TinyUSB
      // descriptor set is composite and built at boot, so CDC survives HID and
      // MSC. Nothing about USB is physically exclusive between them — hence
      // SHARED, and hence these two DO coexist as far as claims are concerned.
      // hid/msc mutual exclusion is a POLICY the modules enforce themselves,
      // not a resource conflict; asserting it here would be asserting a lie.
      {"hid+msc do not conflict on usb (policy, not physics)",
       claim(RES_USB, CLAIM_SHARED),                        // hid
       claim(RES_USB, CLAIM_SHARED, RES_SD, CLAIM_EXCLUSIVE),  // msc
       true, RES_COUNT},
      {"cdc coexists with hid", claim(RES_USB, CLAIM_SHARED), claim(RES_USB, CLAIM_SHARED), true, RES_COUNT},
      {"msc locks out storage", claim(RES_USB, CLAIM_SHARED, RES_SD, CLAIM_EXCLUSIVE), claim(RES_SD, CLAIM_SHARED),
       false, RES_SD},
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
