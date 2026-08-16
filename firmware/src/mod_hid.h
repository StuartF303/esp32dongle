// usbdongle W3 — the `hid` module: a USB HID keyboard for macro / keystroke
// injection into the attached host.
//
// SAFETY POSTURE, up front:
//   * NEVER default-enabled. Arming a keystroke injector is an explicit,
//     reboot-gated act (defaultEnabled=false, bootTimeBinding=true).
//   * Injection (`type`, `key`) requires an AUTHENTICATED session (>= AUTH_TOKEN).
//     CDC is AUTH_PHYSICAL and passes; a fresh Wi-Fi/BLE client (AUTH_NONE) is
//     refused in the MODULE, not the transport — see ARCHITECTURE.md §4.
//   * `release` (releaseAll — the panic stop for a stuck modifier) works at any
//     auth level while the module is enabled, because it can only make the
//     keyboard idle, never inject.
//
// BOOT-TIME BINDING. Its TinyUSB HID interface descriptor is registered from a
// C++ static constructor and the descriptor set is frozen by USB.begin() in
// app_main, before setup(). So the keyboard object is constructed at FILE SCOPE,
// and ONLY when ModulePersist::wasEnabledAtBoot("hid") says the user armed it —
// see the binder in mod_hid.cpp and the TinyUSB block at the top of registry.h.
//
// LAYOUT. HID sends scancodes; the HOST applies the layout. Default en_GB
// (stuart's host is gb/pc105), en_US selectable, persisted in NVS. Layout is a
// property of the host, not the dongle.

#pragma once

#include "registry.h"

// Static descriptor; safe to hand straight to Registry::add().
const ModuleDescriptor *hidModuleDescriptor();
