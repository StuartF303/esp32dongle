// usbdongle W0 — the LittleFS partition, mounted as PLATFORM INFRASTRUCTURE
// (backlog F5).
//
// ---- WHY THIS IS NOT A MODULE -------------------------------------------
//
// `littlefs` at 0x820000 (7.75 MB, see ../partitions.csv and ARCHITECTURE.md
// section 3) is where the web assets, macros and small config files live. It is
// the same class of thing as NVS: something the rest of the image assumes is
// there, mounted once at boot, not something a user turns on and off. Making it
// a module would mean `http` could not serve its own page unless another module
// happened to be enabled, and the enable ordering between the two would become
// a thing anyone had to think about.
//
// `storage` is the ACCESS SURFACE for it (paths under "/fs"), and that is a
// module — but it does not own the mount, and disabling `storage` does not
// unmount LittleFS.
//
// ---- IT MUST NEVER PREVENT BOOT -----------------------------------------
//
// begin() cannot fail in any way that stops setup(). Every outcome is recorded
// and reported through `storage.caps` / `storage.status` and the boot banner.
// A device with a corrupt LittleFS is still a device with a console, an AP and
// an OTA path — which is exactly how it gets fixed.
//
// ---- AND IT MUST NEVER AUTO-FORMAT --------------------------------------
//
// format_if_mount_failed is FALSE and must stay false. An UNFORMATTED partition
// and a CORRUPTED one are indistinguishable at the mount call: both come back
// ESP_FAIL. Formatting on that would mean the first boot after a bad power cut
// silently erases the owner's web assets and macros, with a successful mount as
// the only evidence anything happened. So it reports the failure and waits to
// be told — `storage.format` (AUTH_PHYSICAL) is the explicit action, and it is
// the only thing in the image that erases this partition.

#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace Fs {

// Partition label, exactly as spelled in partitions.csv. Looked up by LABEL and
// never by offset: an offset written down twice is an offset that can disagree
// with the table the bootloader actually read.
constexpr const char *LABEL = "littlefs";

// VFS mountpoint AND the caller-visible volume prefix (volpath.h). One spelling
// for a path all the way from the phone to the syscall.
constexpr const char *MOUNT = "/fs";

enum Stage : uint8_t {
  FS_OK = 0,
  FS_NOT_TRIED,     // begin() has not run
  FS_NO_PARTITION,  // no partition labelled "littlefs" in the live table
  FS_NO_MEM,        // the mount could not allocate its caches
  FS_MOUNT_FAILED,  // the partition is there and would not mount: unformatted OR corrupt
  FS_UNKNOWN,       // esp_vfs_littlefs_register returned something not covered above
};

// Mount it. Call ONCE from setup(). Never fatal, never formats, idempotent.
// Returns true if the volume is mounted when it returns.
bool begin();

bool mounted();
Stage stage();
const char *stageName();
int32_t lastErr();  // the esp_err_t from the failing call, 0 if none
const char *detail();

// Partition geometry, from the live table. 0 if the partition is absent.
uint32_t partitionOffset();
uint32_t partitionSize();

// total/used bytes as LittleFS reports them. False if it is not mounted.
// Cheap enough to call from an explicit action; NOT called from status(),
// which runs on every `modules` command.
bool info(uint64_t *total, uint64_t *used);

// ERASES THE PARTITION. The explicit, clearly-named action behind
// `storage.format p:{volume:"fs", confirm:true}` at AUTH_PHYSICAL — the only
// code path in the image that destroys this filesystem. Unmounts, formats and
// remounts; on success the volume is empty and mounted.
//
// Returns false with *code set to a wire code and `msg` to a sentence.
bool format(const char **code, char *msg, size_t cap);

// Mount state for `storage.status` / `storage.caps`. Reads only.
void fillStatus(JsonObject d);

}  // namespace Fs
