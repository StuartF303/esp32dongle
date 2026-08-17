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

// ---- selecting the next boot partition ----------------------------------

// What was selected, for the response and the bus event. Fixed buffers, no
// pointers into IDF's partition list: the report outlives the lookup and is
// rendered after `d` has been cleared and refilled.
//
// [17] is esp_partition_t::label's own size (16 characters + NUL);
// [48]/[32] match the esp_app_desc_t fields they are built from
// (date[16] + ' ' + time[16], idf_ver[32]).
struct BootSetReport {
  char previous[17];    // the boot partition otadata named BEFORE the write
  char selected[17];    // the one it names now
  char build[48];       // the selected image's "<date> <time>" — WHICH build was chosen
  char idfVersion[32];  // ... and its idf_ver
  uint32_t offset;      // the selected partition's flash offset
  // false == otadata already named this partition. NOT "nothing was written":
  // IDF rewrites the inactive otadata sector either way (same ota_seq,
  // ota_state = NEW), so both sectors then carry the same sequence number and
  // which one wins is a bootloader tie-break. Selecting the slot that is
  // already selected is therefore pointless rather than harmful — worst case
  // the next boot of that same image is a PENDING_VERIFY boot the health check
  // confirms 30 s later.
  bool changed;
  bool rebootRequired;  // false == the selection is the partition already running
};

// esp_ota_set_boot_partition() on the app partition labelled `label`.
//
// DOES NOT REBOOT. Choosing the next image and restarting into it are two
// decisions, and an operator who wants to look at the device between them must
// be able to. Use `reboot` (also AUTH_PHYSICAL) when ready.
//
// Refuses, before touching otadata, when the label is malformed, names no
// partition, names a partition that is not of type app, or names an app
// partition with no readable app descriptor — the same
// esp_ota_get_partition_description() check `rollback_target.has_app` reports.
// That last one is the one that matters: pointing the bootloader at an erased
// slot is how a working device becomes a USB-recovery job, and this command
// exists to make the rollback test SAFE, so it must not be the thing that
// breaks it.
//
// IDF then applies its own, stronger gate — esp_ota_set_boot_partition()
// verifies the whole image (image_validate/ESP_IMAGE_VERIFY) and returns
// ESP_ERR_OTA_VALIDATE_FAILED without writing otadata if the image is
// truncated or corrupt. A half-written slot is refused by one of the two.
//
// On success the selected slot's otadata entry is written with
// ota_state = ESP_OTA_IMG_NEW, so with rollback enabled in the bootloader the
// next boot of that image is a PENDING_VERIFY boot and the machinery above
// runs. That is the point of the command.
bool setBootNow(const char *label, BootSetReport &rep, const char **code, char *msg, size_t cap);

// Render a BootSetReport into the response. Separate from setBootNow() because
// the caller clears and refills `d` from fillStatus() in between.
void fillBootSet(JsonObject d, const BootSetReport &rep);

}  // namespace OtaHealth
