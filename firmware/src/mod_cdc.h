// usbdongle W1 — the `cdc` module: the USB serial console, as a registry
// citizen rather than an invisible special case.
//
// WHY A TRANSPORT IS A MODULE. Before this existed, `enable msc force` would
// look at the module table, find nothing holding USB, cheerfully report
// `stopped: []`, and — once `msc` was real — reconfigure the USB stack out
// from under the very console it had just replied on. The registry cannot
// arbitrate a resource whose principal user is not in the table.
//
// So `cdc` takes RES_USB SHARED and is `essential`: it is force-enabled at
// every boot regardless of NVS, and disable() refuses. A force-enable blocked
// by it now says so by name instead of silently taking the link away.
//
// CLAIM MODEL, CORRECTED (2026-08-16). On this framework the TinyUSB
// descriptor set is composite and built before setup(): CDC, HID and MSC
// coexist on one device. Nothing about USB is physically exclusive between
// them, so all three take RES_USB *SHARED*, and `msc`'s real exclusivity is
// over RES_SD (it hands the card to the host PC as a block device).
//
// hid/msc mutual exclusion, if it is wanted, is therefore a POLICY those
// modules enforce — not a resource conflict. Encoding it as USB-EXCLUSIVE
// would be pretending a physical constraint exists that does not, and would
// also make `cdc` uncoexistable with both, i.e. would break the console.

#pragma once

#include "registry.h"

// Static descriptor; safe to hand straight to Registry::add().
const ModuleDescriptor *cdcModuleDescriptor();
