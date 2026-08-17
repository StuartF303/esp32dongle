// usbdongle W1 — the persisted module-id sets, and the boot decision rule.
//
// THE DEFECT THIS FIXES. The registry persisted ONE list: the ids that are
// enabled ("id1,id2,..."). descriptor.defaultEnabled was then applied only
// when that key was ABSENT — i.e. on a virgin device. So on a device that had
// ever written the key, a module ADDED to the firmware later was, by
// construction, absent from the stored list and therefore off. Forever, with
// nothing anywhere saying why. `display` shipped with defaultEnabled = true
// and came up disabled on stuart's dongle for exactly this reason.
//
// The information that was missing is not "which modules are on" — it is
// "which modules this device has ever HEARD OF". Absent-because-new and
// absent-because-the-owner-turned-it-off are different facts and one list
// cannot hold both. So a second list is persisted: every id registered at the
// last boot. The rule then falls out:
//
//   in the enabled set          -> on   (the owner asked for it)
//   not in the enabled set,
//     and in the known set      -> OFF  (the owner turned it off; it stays off)
//     and NOT in the known set  -> the module's own defaultEnabled (it is new)
//
// MIGRATION IS DELIBERATELY CONSERVATIVE. On a device upgraded from a build
// that never wrote a known set, the known list is absent and every module is
// therefore "new" — which would re-enable every default-on module the owner
// had turned off. That is precisely the outcome the brief forbids, so an
// ABSENT known list is treated as "everything currently registered is known",
// i.e. exactly the old behaviour, for that one boot only. The cost is stated
// plainly: on the first boot after this change, a default-on module that is
// not in the persisted set stays off and has to be enabled once by hand. From
// the boot after that, the rule above applies.
//
// DEPENDENCY-FREE (claims.h's rule): <stddef.h>, <stdint.h>, <string.h>. The
// decision is here rather than inline in registry.cpp so that `pio test -e
// native` tests the rule the device actually runs, not a copy of it.
//
// !! SHARED FORMAT !! The list syntax — "id1,id2,...", no spaces, no trailing
// comma — is also parsed by ModulePersist::wasEnabledAtBoot() from a static
// constructor before the Registry exists. Both go through contains() below so
// there is one parser, not two that can drift.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ModSet {

// Is `id` one of the comma-separated ids in `list`?
//
// Exact, whole-token match: "led" does not match "ledx" and does not match a
// prefix of it. A null or empty list contains nothing.
inline bool contains(const char *list, const char *id) {
  if (list == nullptr || id == nullptr || id[0] == '\0') {
    return false;
  }
  size_t idLen = strlen(id);
  const char *cur = list;
  while (*cur != '\0') {
    const char *comma = strchr(cur, ',');
    size_t len = (comma != nullptr) ? (size_t)(comma - cur) : strlen(cur);
    if (len == idLen && strncmp(cur, id, idLen) == 0) {
      return true;
    }
    if (comma == nullptr) {
      break;
    }
    cur = comma + 1;
  }
  return false;
}

// THE BOOT DECISION for one module. See the block comment above.
//
//   enabledList == nullptr  the enabled key is absent: a virgin device (or an
//                           erased NVS). Every module gets its own default.
//   knownList   == nullptr  the known key is absent: a device upgraded from a
//                           build that did not write one. Conservative — see
//                           the migration note; nothing is resurrected.
//
// `essential` is NOT considered here. The registry forces those on after this
// rule has run, whatever NVS says, including when NVS is unreadable.
inline bool wantAtBoot(const char *enabledList, const char *knownList, const char *id, bool defaultEnabled) {
  if (enabledList == nullptr) {
    return defaultEnabled;
  }
  if (contains(enabledList, id)) {
    return true;
  }
  if (knownList == nullptr) {
    return false;
  }
  if (contains(knownList, id)) {
    return false;  // the owner disabled it. It stays disabled.
  }
  return defaultEnabled;  // this device has never heard of it: it is new.
}

// Appends `id` to a bounded "a,b,c" list, inserting the separator.
//
// Returns FALSE and leaves `buf` byte-for-byte unchanged if it would not fit.
// The previous appendf() truncated silently, which on the persisted set means
// modules quietly stop coming back after a reboot with nothing reporting it.
// `buf` must be NUL-terminated on entry ("" for an empty list).
inline bool append(char *buf, size_t cap, const char *id) {
  if (buf == nullptr || cap == 0 || id == nullptr || id[0] == '\0') {
    return false;
  }
  size_t used = strlen(buf);
  size_t idLen = strlen(id);
  size_t sep = (used > 0) ? 1u : 0u;
  if (used + sep + idLen + 1 > cap) {
    return false;
  }
  if (sep) {
    buf[used] = ',';
  }
  memcpy(buf + used + sep, id, idLen + 1);
  return true;
}

}  // namespace ModSet
