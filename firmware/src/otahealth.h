// usbdongle W0 — OTA rollback confirmation (backlog S4).
//
// The bootloader on this device is ARMED for rollback
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in the prebuilt framework sdkconfig
// — see the block comment in otadecide.h for the evidence). This module is the
// other half: it decides whether the running image has proved itself, and calls
// esp_ota_mark_app_valid_cancel_rollback() when it has.
//
// The decision table itself is in otadecide.h, dependency-free and host-tested.
// Everything in HERE is the part that touches flash, partitions, the registry
// and the clock.
//
// ---- WHAT HAPPENS ON A NORMAL BOOT --------------------------------------
//
// Nothing. PlatformIO writes boot_app0.bin at otadata's offset, which selects
// app0 with ota_state UNDEFINED (see otadecide.h for the byte-level detail), so
// the running partition is never PENDING_VERIFY after a USB flash and begin()
// parks in PHASE_IDLE. tick() is then one load and one branch for the life of
// the image. This code only ever does anything on a boot that followed an OTA.

#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace OtaHealth {

// Call once from setup(), AFTER registry.restoreFromNvs() and after
// Console::begin().
//
// Both orderings are load-bearing:
//   * after restoreFromNvs(), because CRIT_ESSENTIAL asks the registry whether
//     `cdc` came up, and because `display` subscribes to activity.h in its
//     enable() — subscribing after our first Activity::begin() would show
//     nothing on the panel;
//   * after Console::begin(), because that is what registers the CDC sink on
//     the bus, and the "ota.pending" event is meant to land in the boot log.
void begin();

// Scheduler task. Register at OtaDecide::DEFAULTS.tickMs.
void tick();

// Fills the `ota` command's `d`. Read-only, allocates nothing but JSON.
void fillStatus(JsonObject d);

// ---- the two forced actions (AUTH_PHYSICAL — see cmdauth.h) -------------
//
// Both return false on refusal or failure and write a reason into `msg`;
// `*code` is set to the wire error code in that case. On success `msg` carries
// a human sentence for the response's `d`.

// esp_ota_mark_app_valid_cancel_rollback() now, skipping the health check.
// Refuses when the running image is not in PENDING_VERIFY: there is then
// nothing to confirm, and pretending otherwise would report success for a
// no-op.
bool confirmNow(const char **code, char *msg, size_t cap);

// esp_ota_mark_app_invalid_rollback_and_reboot(). DESTRUCTIVE AND TERMINAL —
// on success this function does not return, the chip restarts into the other
// slot.
//
// Refuses unless esp_ota_check_rollback_is_possible() says the other slot holds
// a bootable, otadata-blessed app. IDF refuses too
// (ESP_ERR_OTA_ROLLBACK_FAILED), but it refuses with an error number; rolling
// back into an erased slot is how a working device becomes a USB-recovery job,
// so it is worth saying which of the two things is missing.
bool rollbackNow(const char **code, char *msg, size_t cap);

}  // namespace OtaHealth
