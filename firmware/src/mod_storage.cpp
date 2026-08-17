#include "mod_storage.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdmmc_host.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "activity.h"
#include "b64.h"
#include "bus.h"
#include "cmdauth.h"
#include "crc32.h"
#include "fsmount.h"
#include "modauth.h"
#include "pathsafe.h"
#include "protocol.h"
#include "volpath.h"

namespace {

// ===========================================================================
// Hardware — verified, see ../CLAUDE.md "Board pinout". Do not re-derive.
// ===========================================================================
//
// SDMMC 4-BIT, not SPI. The card is inside the USB-A shell and shares that one
// combined USB-3.0-TYPE-A-TF connector with the USB D+/D- pair. The schematic
// notes pull-ups were added to the SD lines and that the connector's pin order
// is reversed relative to the manufacturer's part — which is why these numbers
// are copied from LilyGO's own example and not guessed from a datasheet.
//
// There is NO CARD-DETECT PIN on this board. "No card fitted" and "card fitted
// but unreadable" are indistinguishable to firmware except by WHICH bring-up
// stage fails, which is the entire reason for the two-stage reporting below.
constexpr int PIN_CLK = 12;
constexpr int PIN_CMD = 16;
constexpr int PIN_D0 = 14;
constexpr int PIN_D1 = 17;
constexpr int PIN_D2 = 21;
constexpr int PIN_D3 = 18;

// ===========================================================================
// TWO VOLUMES, ONE SURFACE (backlog F5)
// ===========================================================================
//
// This module addresses BOTH filesystems through one chunked read/write/CRC/
// list/stat/mkdir/delete/verify surface:
//
//   /sd/...   the microSD card, mounted and unmounted by THIS module's
//             enable()/disable() (SDMMC 4-bit, see the pinout above).
//   /fs/...   the LittleFS partition at 0x820000, mounted at BOOT by
//             Fs::begin() and NOT owned by this module at all (fsmount.h).
//             Disabling `storage` does not unmount it; it only takes away the
//             command surface that reaches it.
//
// WHAT THE PREFIX DOES TO PathSafe: nothing. It sits strictly in front of it.
// VolPath::split() peels "/sd" or "/fs" off and hands back the path WITHIN the
// volume, which always begins with '/', and PathSafe::check() then runs on that
// exactly as it always has. There is ONE path checker in this image, it is the
// one with the adversarial test suite behind it, and this file did not gain a
// second one — see the block at the top of volpath.h.
//
// THE PREFIX AND THE VFS MOUNTPOINT ARE THE SAME STRING, deliberately, so a
// path has one spelling from the phone all the way to the syscall. That makes
// the composition below look like a no-op, and it is NOT one: the full path is
// always rebuilt from the volume's own mountpoint constant plus the REST that
// PathSafe accepted. Passing the caller's buffer to a syscall because it
// happens to be byte-identical is how a checker gets bypassed by an edit that
// looks harmless.
enum VolId : uint8_t { VOL_SD = 0, VOL_FS = 1, VOL_COUNT = 2 };

struct VolumeDef {
  const char *name;    // wire-visible volume name, no slash
  const char *mount;   // VFS mountpoint == caller-visible prefix
  uint8_t mountLen;
  const char *fsName;  // filesystem family, for caps/status
};

constexpr VolumeDef VOLUMES[VOL_COUNT] = {
    {"sd", "/sd", 3, "fat"},
    {"fs", "/fs", 3, "littlefs"},
};
static_assert(sizeof("/sd") - 1 == 3, "VOL_SD mountLen must match its mountpoint");
static_assert(sizeof("/fs") - 1 == 3, "VOL_FS mountLen must match its mountpoint");
// fsmount.h owns the LittleFS mountpoint; if the two ever disagree, paths would
// resolve to a directory that is not the one anybody mounted.
static_assert(CmdAuth::streq("/fs", Fs::MOUNT), "VOL_FS mountpoint must match Fs::MOUNT");

// Longest mountpoint, for the path buffer below.
constexpr size_t MAX_MOUNT_LEN = 3;

// FatFs file descriptors the mount reserves. `verify` holds one open across
// ticks; everything else opens and closes inside a single dispatch.
constexpr uint8_t MAX_OPEN_FILES = 5;

// ===========================================================================
// Bounds. EVERY one of these is advertised through the `caps` action, because
// a caller that has to discover a limit by hitting it will discover it halfway
// through a 200 MB upload.
// ===========================================================================

// Raw bytes per read/write chunk. Derived from Protocol::MAX_LINE, not picked:
// base64 inflates 4/3, so 2048 raw becomes 2732 characters, and the JSON
// envelope (id/ok/d/path/offset/bytes/eof/crc32) is comfortably under the ~1.3
// KB that leaves inside a 4096-byte line.
constexpr size_t MAX_CHUNK = 2048;
constexpr size_t B64_CHUNK_LEN = B64::encodedLen(MAX_CHUNK);  // 2732
// The guard has to budget for the WHOLE worst-case response, not just the
// payload: a read echoes p.path back, and that can be a volume prefix plus
// MAX_PATH_LEN bytes on its own. 2732 + 9 + 255 + 200 (id/ok/d/offset/len/size/
// bytes/crc32/eof and the punctuation) = 3196, inside 4096. Raising MAX_CHUNK
// without raising MAX_LINE now fails the build instead of producing a response
// no transport can frame.
static_assert(B64_CHUNK_LEN + VolPath::MAX_PREFIX_LEN + PathSafe::MAX_PATH_LEN + 200 < Protocol::MAX_LINE,
              "a full max_chunk read response would not fit inside Protocol::MAX_LINE");

// Directory listing. BOTH bounds are enforced: `limit` caps the entry count,
// and LIST_BYTE_BUDGET caps the serialised size, because a page of 32 entries
// with 200-byte names would blow the line limit while satisfying the count.
constexpr uint16_t LIST_DEFAULT_LIMIT = 32;
constexpr uint16_t LIST_MAX_LIMIT = 64;
constexpr size_t LIST_BYTE_BUDGET = 2800;
constexpr size_t LIST_ENTRY_OVERHEAD = 72;  // {"name":"","size":N,"is_dir":false,"mtime":N},
// readdir() has no seek, so an offset is reached by skipping. Bounded so a
// caller cannot ask us to walk a directory forever.
constexpr uint32_t LIST_MAX_OFFSET = 65535;

// Recursive delete. Refuses and reports rather than running away.
constexpr uint8_t DELETE_MAX_DEPTH = 8;
constexpr uint32_t DELETE_MAX_ENTRIES = 2000;

// FatFs file offsets are off_t, which is 32-bit signed on this target, so no
// byte past 2 GiB - 1 is addressable through this API at all. Stated rather
// than discovered: a FAT32 volume can legally hold a 4 GiB - 1 file.
constexpr uint32_t MAX_FILE_OFFSET = 2147483647u;

// `verify` pump budget. Two bounds, because either alone is wrong: a byte
// budget alone stalls the loop if the card is slow, and a time budget alone
// makes throughput depend on how long millis() happens to take to tick.
constexpr size_t VERIFY_SLICE = 1024;            // one fread
constexpr uint32_t VERIFY_BYTES_PER_TICK = 16384;  // 16 slices
constexpr uint32_t VERIFY_MS_PER_TICK = 5;
constexpr uint32_t VERIFY_PROGRESS_MS = 500;
constexpr uint32_t VERIFY_TICK_MS = 5;

// ===========================================================================
// Buffers
// ===========================================================================
//
// File scope, not stack: the loop task's stack is 8 KB and the recursive delete
// already wants ~1.6 KB of it.
//
// chunkBuf is SHARED between read/write chunking and the `verify` pump. That is
// safe, and the reason is a registry invariant rather than luck: Registry::
// dispatch() and Registry::tickAt() both take the same recursive mutex and both
// run on the loop task, so a tick can never interleave with a dispatch (see the
// THREADING block in registry.h). If module ticks ever move to their own task,
// this sharing must be split — that is the one thing to check.
uint8_t chunkBuf[MAX_CHUNK];
char b64Buf[B64_CHUNK_LEN + 1];

// ===========================================================================
// Mount state and the two-stage failure story
// ===========================================================================
//
// ESP-IDF separates card bring-up from filesystem mount, and so must we. This
// project has already lost time to conflating them: the boot log said
// `mount_to_vfs failed (0xffffffff)`, NOT `sdmmc_card_init failed`, and that
// single distinction was the whole proof that the card was present and talking
// and only FatFs was refusing it — because 128 GB cards ship exFAT and
// Arduino-ESP32 compiles exFAT out (FF_FS_EXFAT is 0 in this toolchain's
// ffconf.h; verified 2026-08-16). See ../CLAUDE.md "microSD — resolved".
//
// Arduino's SD_MMC.begin() throws that distinction away: it collapses
// esp_vfs_fat_sdmmc_mount()'s esp_err_t into a bool. So on failure — and ONLY
// on failure, so the happy path pays nothing — we re-run the card
// initialisation alone. If the card inits, the failure was the filesystem and
// the answer is "reformat it FAT32". If it does not, the failure was the card
// and the answer is "check the card is fitted".
enum MountStage : uint8_t {
  STAGE_OK = 0,
  STAGE_PINS,     // SD_MMC.setPins() refused the pin set
  STAGE_CARD,     // no card, bad contacts, or the card will not answer CMD0/ACMD41
  STAGE_FS,       // the card is fine; FatFs would not mount the volume (exFAT, or corrupt)
  STAGE_UNKNOWN,  // the probe itself could not run, so we will not guess
};

// SD ONLY. LittleFS keeps its own state in fsmount.cpp and is not owned here.
bool sdMounted = false;
MountStage lastStage = STAGE_OK;
int32_t lastStageErr = 0;
// Short form, handed to the registry as the enable() failure reason. It is
// interpolated into ModuleActionResult::msg (192 bytes) alongside ~50 bytes of
// registry wording, so it has to stay under ~130 characters. The long form
// lives in mountDetail and comes back through status().
char enableErr[144];
char mountDetail[288];

const char *stageName(MountStage s) {
  switch (s) {
    case STAGE_OK:
      return "ok";
    case STAGE_PINS:
      return "pins";
    case STAGE_CARD:
      return "card_init";
    case STAGE_FS:
      return "fs_mount";
    default:
      return "unknown";
  }
}

// Runs ONLY after SD_MMC.begin() has already failed and cleaned up after
// itself (esp_vfs_fat_sdmmc_mount deinitialises the host on every error path,
// and Arduino's begin() has not yet claimed the pins with the peripheral
// manager at that point, so the bus is free).
MountStage probeFailedStage(int32_t *errOut) {
  *errOut = 0;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_4BIT;
  host.slot = SDMMC_HOST_SLOT_1;
  // Probe at the conservative 20 MHz default, not high speed: the question here
  // is "does the card answer at all", and answering it at a lower clock removes
  // signal integrity from the list of possible explanations.
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)PIN_CLK;
  slot.cmd = (gpio_num_t)PIN_CMD;
  slot.d0 = (gpio_num_t)PIN_D0;
  slot.d1 = (gpio_num_t)PIN_D1;
  slot.d2 = (gpio_num_t)PIN_D2;
  slot.d3 = (gpio_num_t)PIN_D3;
  slot.width = 4;

  esp_err_t e = host.init();
  if (e != ESP_OK) {
    // Almost always ESP_ERR_INVALID_STATE, i.e. the host was left initialised
    // by whatever failed. Reporting STAGE_CARD here would be a guess dressed
    // up as a diagnosis, so it is not made.
    *errOut = (int32_t)e;
    return STAGE_UNKNOWN;
  }
  e = sdmmc_host_init_slot(host.slot, &slot);
  if (e != ESP_OK) {
    host.deinit();
    *errOut = (int32_t)e;
    return STAGE_UNKNOWN;
  }

  sdmmc_card_t *card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
  if (card == nullptr) {
    host.deinit();
    return STAGE_UNKNOWN;
  }
  e = sdmmc_card_init(&host, card);
  free(card);
  host.deinit();

  *errOut = (int32_t)e;
  return (e == ESP_OK) ? STAGE_FS : STAGE_CARD;
}

// FatFs's own view of the volume. This is the one call that can say "exfat" out
// loud — and on this toolchain it never will, because FF_FS_EXFAT is 0, so an
// exFAT card fails at STAGE_FS and never reaches here. Reported anyway so the
// answer comes from the filesystem rather than from an assumption.
//
// CACHED, and deliberately: the only way to reach FATFS::fs_type from here is
// f_getfree(), which on FAT32 can walk the entire FAT when the volume's FSInfo
// free-cluster count is stale — millions of clusters on a 128 GB card. The
// filesystem type cannot change while the volume is mounted, so it is read once
// at mount and returned from RAM thereafter. status() runs on every `modules`
// command and must not be able to stall the loop for seconds.
const char *fsTypeCached = "?";

const char *readFsType() {
  FATFS *fs = nullptr;
  DWORD freeClust = 0;
  // Drive "0:" matches what SD_MMC's own totalBytes()/usedBytes() use: the SD
  // volume is the only FatFs mount in this image (LittleFS is not FatFs).
  if (f_getfree("0:", &freeClust, &fs) != FR_OK || fs == nullptr) {
    return "?";
  }
  switch (fs->fs_type) {
    case FS_FAT12:
      return "fat12";
    case FS_FAT16:
      return "fat16";
    case FS_FAT32:
      return "fat32";
    case FS_EXFAT:
      return "exfat";
    default:
      return "?";
  }
}

const char *cardTypeName() {
  switch (SD_MMC.cardType()) {
    case CARD_NONE:
      return "none";
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SD";
    case CARD_SDHC:
      return "SDHC";
    default:
      return "unknown";
  }
}

// ===========================================================================
// Path handling — VolPath then PathSafe, and NOTHING else touches a path
// ===========================================================================

// Full VFS path buffer: the longest mountpoint + the longest path PathSafe will
// accept + NUL.
constexpr size_t FULL_PATH_MAX = MAX_MOUNT_LEN + PathSafe::MAX_PATH_LEN + 1;

// One resolved path. Everything a handler needs to act, and nothing it has to
// re-derive: re-deriving "is this the volume root" from the string is exactly
// the kind of second parse this file is meant not to have.
struct Resolved {
  uint8_t vol = VOL_COUNT;
  bool isRoot = false;
  char full[FULL_PATH_MAX] = {0};
};

bool volumeMounted(uint8_t v) {
  switch (v) {
    case VOL_SD:
      return sdMounted;
    case VOL_FS:
      return Fs::mounted();
    default:
      return false;
  }
}

// Human noun for a volume, used in the errno advice so "the card is full" does
// not appear when it is the 7.75 MB flash partition that filled up.
const char *volNoun(uint8_t v) { return v == VOL_FS ? "the LittleFS partition" : "the card"; }

// Lists the volumes into `d` so a rejection tells the caller what IS valid,
// rather than only what is not.
void listVolumes(JsonObject d) {
  JsonArray a = d["volumes"].to<JsonArray>();
  for (uint8_t i = 0; i < VOL_COUNT; i++) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = VOLUMES[i].name;
    o["prefix"] = VOLUMES[i].mount;
    o["mounted"] = volumeMounted(i);
  }
}

// Why a known volume is not mounted, in one actionable sentence. The two
// volumes fail for completely different reasons and a shared "not mounted"
// would hide both.
void volumeDownDetail(uint8_t v, JsonObject d, CmdError *err) {
  if (v == VOL_FS) {
    d["mount_stage"] = Fs::stageName();
    d["detail"] = Fs::detail();
    cmdErrorf(err, "ENOTMOUNTED",
              "volume \"/fs\" is not mounted (LittleFS stage: %s); it is unformatted or corrupt — see storage.status",
              Fs::stageName());
    return;
  }
  d["mount_stage"] = stageName(lastStage);
  d["detail"] = (const char *)mountDetail;
  cmdErrorf(err, "ENOTMOUNTED",
            "volume \"/sd\" is not mounted (last bring-up stage: %s); disable and re-enable storage",
            stageName(lastStage));
}

// Validates a caller-supplied path and builds the VFS path for it. THE funnel:
// every syscall in this file is reached through here and through nothing else.
//
// Three refusals, three distinct codes:
//   EPATH        the shape is wrong (either half of the check said so)
//   EVOLUME      well-formed, but names a volume this firmware does not have
//   ENOTMOUNTED  a real volume that is not currently mounted
//
// On rejection this fills `d` with BOTH the machine-readable token and the full
// human sentence, and sets err's message to the same sentence. `d` survives an
// error response (ARCHITECTURE.md section 2), which matters because
// CmdError::msg is 96 bytes and several of these messages are longer — the
// truncated one goes in `e.msg`, the whole one in `d.path_message`.
//
// The offending path is deliberately NOT echoed back: the checks tolerate a
// buffer with no NUL inside the limit, and echoing it would then read past it.
bool resolvePath(const char *path, Resolved *out, JsonObject d, CmdError *err) {
  VolPath::Split sp;
  VolPath::Result vr = VolPath::split(path, &sp);
  if (vr != VolPath::VOLPATH_OK) {
    d["path_error"] = VolPath::resultName(vr);
    d["path_message"] = VolPath::resultMessage(vr);
    listVolumes(d);
    cmdErrorf(err, "EPATH", "%s", VolPath::resultMessage(vr));
    return false;
  }

  // Name -> index. A linear walk over two entries; a map would be a data
  // structure standing in for an if.
  uint8_t v = VOL_COUNT;
  for (uint8_t i = 0; i < VOL_COUNT; i++) {
    if (strcmp(sp.volume, VOLUMES[i].name) == 0) {
      v = i;
      break;
    }
  }
  if (v == VOL_COUNT) {
    // DISTINCT from EPATH: "/nope/x" is a perfectly well-formed path naming a
    // volume that does not exist, and telling a caller their path is malformed
    // when it is not sends them looking in the wrong place.
    d["volume"] = (const char *)sp.volume;
    listVolumes(d);
    cmdErrorf(err, "EVOLUME", "no volume named \"%.8s\"; this device has /sd (microSD) and /fs (LittleFS)", sp.volume);
    return false;
  }

  // THE path check, unchanged and unduplicated. It sees the path WITHIN the
  // volume, which is exactly the string whose shape it was written to police.
  PathSafe::Result r = PathSafe::check(sp.rest);
  if (r != PathSafe::PATH_OK) {
    d["path_error"] = PathSafe::resultName(r);
    d["path_message"] = PathSafe::resultMessage(r);
    d["volume"] = VOLUMES[v].name;
    cmdErrorf(err, "EPATH", "%s", PathSafe::resultMessage(r));
    return false;
  }

  if (!volumeMounted(v)) {
    d["volume"] = VOLUMES[v].name;
    volumeDownDetail(v, d, err);
    return false;
  }

  // Composed from the VOLUME'S OWN mountpoint plus the accepted remainder —
  // never from the caller's buffer, even though the two are byte-identical
  // here. The root is the mountpoint alone: "/sd" + "/" would be "/sd/", a
  // second spelling of the same directory.
  int n = sp.isRoot ? snprintf(out->full, sizeof(out->full), "%s", VOLUMES[v].mount)
                    : snprintf(out->full, sizeof(out->full), "%s%s", VOLUMES[v].mount, sp.rest);
  if (n <= 0 || (size_t)n >= sizeof(out->full)) {
    // Unreachable given MAX_PATH_LEN and FULL_PATH_MAX, but a silent truncation
    // here would be a path-confusion bug, so it is a hard failure.
    cmdErrorf(err, "EPATH", "resolved path did not fit the %u-byte buffer", (unsigned)sizeof(out->full));
    return false;
  }
  out->vol = v;
  out->isRoot = sp.isRoot;
  return true;
}

// ===========================================================================
// errno -> a distinct wire code and a sentence that says what to do
// ===========================================================================

const char *errnoCode(int e) {
  switch (e) {
    case ENOENT:
      return "ENOENT";
    case EISDIR:
      return "EISDIR";
    case ENOTDIR:
      return "ENOTDIR";
    case ENOTEMPTY:
      return "ENOTEMPTY";
    case EEXIST:
      return "EEXIST";
    case EACCES:
      return "EACCES";
    case EROFS:
      return "EROFS";
    case ENOSPC:
      return "ENOSPC";
    case EMFILE:
    case ENFILE:
      return "EMFILE";
    case ENAMETOOLONG:
      return "ENAMETOOLONG";
    case EINVAL:
      return "EINVAL";
    default:
      return "EIO";
  }
}

// Volume-aware, because the two filesystems fail for different reasons and the
// remedy differs: "the card is full" and "the 7.75 MB flash partition is full"
// send someone to two different places, and ENAMETOOLONG means 255 bytes on
// FatFs but 64 on this LittleFS build (CONFIG_LITTLEFS_OBJ_NAME_LEN).
const char *errnoAdvice(int e, uint8_t vol) {
  bool fs = (vol == VOL_FS);
  switch (e) {
    case ENOENT:
      return fs ? "no such file or directory on the LittleFS partition" : "no such file or directory on the card";
    case EISDIR:
      return "that path is a directory, not a file";
    case ENOTDIR:
      return "a component of that path is a file, not a directory";
    case ENOTEMPTY:
      return "the directory is not empty; pass recursive:true to delete its contents too";
    case EEXIST:
      return "it already exists";
    case EACCES:
      return fs ? "the filesystem refused access"
                : "the filesystem refused access (a read-only or hidden/system FAT attribute)";
    case EROFS:
      return "the volume is mounted read-only";
    case ENOSPC:
      return fs ? "the LittleFS partition is full (7.75 MB total — bulk data belongs on /sd)" : "the card is full";
    case EMFILE:
    case ENFILE:
      return "too many files are open; a verify job may be holding one";
    case ENAMETOOLONG:
      return fs ? "the name is longer than this LittleFS build allows (64 bytes per component)"
                : "the name is longer than FatFs allows";
    default:
      return fs ? "LittleFS reported an I/O error" : "the card reported an I/O error";
  }
}

void failErrno(const char *what, const char *path, int e, uint8_t vol, CmdError *err) {
  cmdErrorf(err, errnoCode(e), "%s \"%.64s\": %s (errno %d)", what, path, errnoAdvice(e, vol), e);
}

// ===========================================================================
// Auth
// ===========================================================================
//
// requireAuth() and requirePhysical() USED TO LIVE HERE and are gone (backlog
// S6). Both are now declarations in modauth.h, applied by Registry::dispatch()
// before this module is entered:
//
//   caps/free/list/stat/read/verify/write/mkdir/delete/status  AUTH_TOKEN
//   format                                                     AUTH_PHYSICAL
//
// The levels are unchanged, including format's — reading someone's card over
// an open AP is not meaningfully less serious than writing to it, so both tiers
// were one bar anyway, and `format` erases a whole filesystem, which stays at
// the cable rather than the network. What the module lost is the ABILITY to
// disagree with the table, and `status`, which had no check at all.

// ===========================================================================
// verify — the only action that outlives its dispatch
// ===========================================================================
//
// FILE HANDLE LIFETIME, stated because the hard requirement asks for it: this
// is the ONE open handle the module keeps between dispatch calls. It is opened
// by `verify`, advanced by the tick, and closed at exactly three places —
// normal completion, cancellation (explicit or by disable()), and any read
// error. Nothing else in this file holds a descriptor past its own return.
//
// Cancellation, documented because the brief left the choice open:
//   * `verify` with p:{cancel:true} cancels the running job and returns OK;
//   * disable() cancels it (and closes the handle before SD_MMC.end());
//   * a SECOND `verify` while one is running is REFUSED with EBUSY naming the
//     running job. It does NOT silently pre-empt: a caller waiting on a
//     progress stream would otherwise see it stop with no event saying why,
//     and "my request cancelled someone else's" is not a default worth having.
struct VerifyJob {
  bool active = false;
  uint32_t id = 0;
  uint32_t reqId = 0;
  FILE *fp = nullptr;
  uint32_t crc = Crc32::INIT;
  uint32_t bytes = 0;
  uint32_t size = 0;
  uint32_t lastProgressMs = 0;
  uint8_t vol = VOL_COUNT;
  // The CALLER'S path, volume prefix and all, so the progress and done events
  // name the same string the request did. Sized for the prefixed form.
  char path[VolPath::MAX_TOTAL_LEN + 1] = {0};

  // Outcome of the most recent job; kept after active goes false so the done
  // event and status() can both read it.
  bool haveResult = false;
  bool resultOk = false;
  uint32_t resultCrc = 0;
  const char *resultCode = nullptr;  // static string, or nullptr on success
  char resultMsg[96] = {0};
};

VerifyJob verify;
uint32_t verifyJobCounter = 0;

void fillVerifyProgress(JsonObject d, void *ctx) {
  (void)ctx;
  d["mod"] = "storage";
  d["job"] = verify.id;
  if (verify.reqId != 0) {
    d["id"] = verify.reqId;  // correlate with the ACCEPTED response
  }
  d["path"] = (const char *)verify.path;
  d["volume"] = verify.vol < VOL_COUNT ? VOLUMES[verify.vol].name : "?";
  d["bytes"] = verify.bytes;
  d["size"] = verify.size;
  d["pct"] = verify.size ? (uint32_t)((uint64_t)verify.bytes * 100u / verify.size) : 100u;
}

void fillVerifyDone(JsonObject d, void *ctx) {
  (void)ctx;
  d["mod"] = "storage";
  d["job"] = verify.id;
  if (verify.reqId != 0) {
    d["id"] = verify.reqId;
  }
  d["path"] = (const char *)verify.path;
  d["volume"] = verify.vol < VOL_COUNT ? VOLUMES[verify.vol].name : "?";
  d["bytes"] = verify.bytes;
  d["size"] = verify.size;
  d["ok"] = verify.resultOk;
  if (verify.resultOk) {
    char hex[9];
    Crc32::toHex8(verify.resultCrc, hex);
    d["crc32"] = (const char *)hex;
  } else {
    d["code"] = verify.resultCode ? verify.resultCode : "EFAIL";
    d["msg"] = (const char *)verify.resultMsg;
  }
}

// Tears the job down and emits storage.verify.done. `code == nullptr` means it
// finished normally.
void finishVerify(const char *code, const char *msg) {
  if (verify.fp != nullptr) {
    fclose(verify.fp);
    verify.fp = nullptr;
  }
  verify.active = false;
  verify.haveResult = true;
  verify.resultOk = (code == nullptr);
  verify.resultCode = code;
  verify.resultCrc = Crc32::finish(verify.crc);
  snprintf(verify.resultMsg, sizeof(verify.resultMsg), "%s", msg ? msg : "");
  Activity::end(verify.resultOk);  // out-of-band progress; see activity.h
  Bus::emit("storage.verify.done", fillVerifyDone, nullptr);
}

void storageTick() {
  if (!verify.active) {
    return;
  }
  uint32_t startMs = millis();
  uint32_t budget = 0;

  while (budget < VERIFY_BYTES_PER_TICK) {
    size_t n = fread(chunkBuf, 1, VERIFY_SLICE, verify.fp);
    if (n > 0) {
      verify.crc = Crc32::update(verify.crc, chunkBuf, n);
      verify.bytes += (uint32_t)n;
      budget += (uint32_t)n;
    }
    if (n < VERIFY_SLICE) {
      if (ferror(verify.fp)) {
        finishVerify("EIO", "read error part-way through the file; the crc32 is incomplete and was not reported");
        return;
      }
      finishVerify(nullptr, "");
      return;
    }
    if ((uint32_t)(millis() - startMs) >= VERIFY_MS_PER_TICK) {
      break;  // time budget: never hold the cooperative scheduler
    }
  }

  // Out-of-band progress (activity.h). Unrate-limited on purpose: it is a
  // no-op unless something is rendering it, and it only moves its own change
  // counter when the whole percentage actually changes — so the LCD animates
  // smoothly without this module knowing an LCD exists, and without borrowing
  // the event bus's VERIFY_PROGRESS_MS pacing, which is tuned for the wire.
  Activity::progressBytes(verify.bytes, verify.size);

  uint32_t now = millis();
  if ((uint32_t)(now - verify.lastProgressMs) >= VERIFY_PROGRESS_MS) {
    verify.lastProgressMs = now;
    Bus::emit("storage.verify.progress", fillVerifyProgress, nullptr);
  }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

// ---- the SD half of enable() ---------------------------------------------
//
// Split out because enable() is now allowed to succeed WITHOUT it. Returns
// true if the card is mounted when it returns; fills lastStage/enableErr/
// mountDetail either way.
bool mountSd() {
  if (sdMounted) {
    return true;
  }
  lastStage = STAGE_OK;
  lastStageErr = 0;
  enableErr[0] = '\0';
  mountDetail[0] = '\0';

  if (!SD_MMC.setPins(PIN_CLK, PIN_CMD, PIN_D0, PIN_D1, PIN_D2, PIN_D3)) {
    lastStage = STAGE_PINS;
    snprintf(enableErr, sizeof(enableErr),
             "stage=pins: SD_MMC.setPins(12,16,14,17,21,18) was refused (the bus is already begun?)");
    snprintf(mountDetail, sizeof(mountDetail),
             "SD_MMC.setPins() must be called before SD_MMC.begin(); it refuses once a card is mounted. Something "
             "else in this image has already begun the SDMMC bus.");
    return false;
  }

  // format_if_mount_failed is FALSE and must stay false. Passing true would let
  // an unreadable card be silently reformatted — destroying the user's data as
  // a side effect of enabling a module.
  if (!SD_MMC.begin(VOLUMES[VOL_SD].mount, /*mode1bit=*/false, /*format_if_mount_failed=*/false, BOARD_MAX_SDMMC_FREQ,
                    MAX_OPEN_FILES)) {
    lastStage = probeFailedStage(&lastStageErr);
    switch (lastStage) {
      case STAGE_FS:
        snprintf(enableErr, sizeof(enableErr),
                 "stage=fs_mount: the card responds but FatFs will not mount it — almost certainly exFAT");
        snprintf(mountDetail, sizeof(mountDetail),
                 "Card init SUCCEEDED and the FAT mount FAILED, so the card is fitted and talking. Arduino-ESP32 "
                 "compiles exFAT OUT (FF_FS_EXFAT is 0), and 128 GB SDXC cards ship exFAT-formatted. Reformat FAT32 "
                 "(mkfs.vfat -F 32 -s 64, MBR type 0x0c). Do not chase a dead card.");
        break;
      case STAGE_CARD:
        snprintf(enableErr, sizeof(enableErr), "stage=card_init: no card answered on the SDMMC bus (esp_err 0x%x)",
                 (unsigned)lastStageErr);
        snprintf(mountDetail, sizeof(mountDetail),
                 "Card init FAILED, so nothing answered CMD0/ACMD41. There is no card-detect pin on this board, so "
                 "'no card fitted' and 'card fitted but not responding' look identical here. Check the card is "
                 "seated in the USB-A shell and that its contacts are clean.");
        break;
      case STAGE_PINS:
      default:
        lastStage = STAGE_UNKNOWN;
        snprintf(enableErr, sizeof(enableErr), "stage=unknown: mount failed and the follow-up probe could not run");
        snprintf(mountDetail, sizeof(mountDetail),
                 "SD_MMC.begin() failed and the card-init probe could not be run (esp_err 0x%x), so the failing "
                 "stage is genuinely unknown and is not being guessed at. Retry, or reboot the device.",
                 (unsigned)lastStageErr);
        break;
    }
    return false;
  }

  sdMounted = true;
  lastStage = STAGE_OK;
  fsTypeCached = readFsType();
  snprintf(mountDetail, sizeof(mountDetail), "mounted at %s, %s, %s", VOLUMES[VOL_SD].mount, cardTypeName(),
           fsTypeCached);
  return true;
}

// Fires on every enable, success or not, so the SD diagnosis is visible on
// every transport rather than only to whoever typed `enable storage`.
struct MountEvent {
  bool sd;
  bool fs;
  const char *stage;
};

void fillMountEvent(JsonObject d, void *ctx) {
  const MountEvent *e = (const MountEvent *)ctx;
  if (e == nullptr) {
    return;
  }
  d["sd"] = e->sd;
  d["fs"] = e->fs;
  d["sd_stage"] = e->stage;
  if (!e->sd) {
    d["sd_detail"] = (const char *)mountDetail;
  }
}

// ---- enable / disable ----------------------------------------------------
//
// SUCCEEDS ON EITHER VOLUME, and that is the point of the change: an absent
// microSD card must not take the LittleFS partition away with it, and a
// LittleFS that will not mount must not stop someone reading their card.
//
// It fails only when NEITHER is usable, because at that point the module has
// no filesystem to offer and reporting "enabled" would be a lie the UI would
// happily render.
//
// WHAT THIS DOES NOT CHANGE: the RES_SD claim. Claims are static in this
// registry (registry.h), so `storage` holds RES_SD SHARED whenever it is
// enabled, card or no card. The cost, stated rather than discovered: `msc`
// (backlog F2, RES_SD EXCLUSIVE) will be blocked by an enabled `storage` even
// when there is no card in the slot, and the remedy is `disable storage` or
// `enable msc force`. The alternative — dropping the claim when the mount
// fails — needs dynamic claims AND would race a card inserted a second later,
// which is a worse trade for a device with no card-detect pin.
bool storageEnable(const char **errMsg) {
  bool sd = mountSd();
  // NOT mounted here. Fs::begin() ran at boot as platform infrastructure
  // (fsmount.h); this module only reads its state. Enabling `storage` must not
  // be able to mount, format or otherwise touch the LittleFS volume.
  bool fs = Fs::mounted();

  MountEvent ev = {sd, fs, stageName(lastStage)};
  Bus::emit("storage.mount", fillMountEvent, &ev);

  if (!sd && !fs) {
    // Both halves in one message, because "no card" alone would send someone
    // looking for a card when the LittleFS partition is the thing that broke.
    snprintf(enableErr, sizeof(enableErr),
             "no volume is usable: /sd stage=%s, /fs stage=%s — storage.status carries the full diagnosis for both",
             stageName(lastStage), Fs::stageName());
    *errMsg = enableErr;
    return false;
  }
  return true;
}

bool storageDisable(const char **errMsg) {
  (void)errMsg;
  // Close the one long-lived handle BEFORE unmounting: esp_vfs_fat_sdcard_unmount
  // with a file still open leaks the FatFs descriptor for the life of the image,
  // and the next enable() would start one short of MAX_OPEN_FILES. The handle
  // may belong to EITHER volume — a verify running on /fs has to be closed too,
  // even though nothing here unmounts /fs.
  if (verify.active) {
    finishVerify("ECANCELLED", "storage was disabled while this verify job was running");
  }
  if (sdMounted) {
    SD_MMC.end();  // unmounts and releases the pins, so `msc` can claim RES_SD
    sdMounted = false;
  }
  fsTypeCached = "?";  // the next mount may be a different card entirely
  lastStage = STAGE_OK;
  // LittleFS is DELIBERATELY left mounted: it is platform infrastructure
  // (fsmount.h), other code may be reading web assets out of it, and this
  // module never mounted it in the first place.
  snprintf(mountDetail, sizeof(mountDetail), "unmounted cleanly; the SDMMC bus is free. /fs is unaffected.");
  return true;
}

// ===========================================================================
// Actions
// ===========================================================================

// One volume's row, for `caps` and `free`. `withUsage` is the expensive half:
// on /sd it is SD_MMC.totalBytes()/usedBytes(), i.e. f_getfree(), which walks
// the whole FAT when the FAT32 FSInfo free-cluster count is stale — seconds on
// a 128 GB card. Both callers are explicit actions, so they pay it; status()
// must never ask for it.
void fillVolume(JsonObject o, uint8_t v, bool withUsage) {
  o["name"] = VOLUMES[v].name;
  o["prefix"] = VOLUMES[v].mount;
  bool up = volumeMounted(v);
  o["mounted"] = up;

  if (v == VOL_SD) {
    o["kind"] = "microsd";
    o["fs"] = up ? fsTypeCached : "?";
    o["removable"] = true;
    o["stage"] = stageName(lastStage);
    // FatFs LFN, and off_t is 32-bit signed on this target — a legal 4 GiB-1
    // FAT32 file is not fully addressable through this API (backlog C8).
    o["max_name"] = (uint32_t)PathSafe::MAX_SEGMENT_LEN;
    o["max_file_offset"] = MAX_FILE_OFFSET;
    if (up) {
      o["card_type"] = cardTypeName();
      o["card_size"] = SD_MMC.cardSize();
      o["sector_size"] = SD_MMC.sectorSize();
      if (withUsage) {
        uint64_t total = SD_MMC.totalBytes();
        uint64_t used = SD_MMC.usedBytes();
        o["total"] = total;
        o["used"] = used;
        o["free"] = (total >= used) ? (total - used) : 0;
      }
    }
    return;
  }

  o["kind"] = "littlefs";
  o["fs"] = VOLUMES[v].fsName;
  o["removable"] = false;
  o["stage"] = Fs::stageName();
  o["offset"] = Fs::partitionOffset();
  o["partition_size"] = Fs::partitionSize();
  // CONFIG_LITTLEFS_OBJ_NAME_LEN in this framework's sdkconfig. Lower than
  // PathSafe::MAX_SEGMENT_LEN, so a name legal on /sd can be ENAMETOOLONG here
  // — advertised rather than discovered halfway through an upload.
  o["max_name"] = 64u;
  // The partition is 7.75 MB, so the 2 GiB off_t ceiling is never the binding
  // limit on this volume; the partition size is.
  o["max_file_offset"] = Fs::partitionSize();
  if (up && withUsage) {
    uint64_t total = 0, used = 0;
    if (Fs::info(&total, &used)) {
      o["total"] = total;
      o["used"] = used;
      o["free"] = (total >= used) ? (total - used) : 0;
    }
  }
}

DispatchResult actCaps(JsonObject d) {
  d["encoding"] = "base64";  // RFC 4648 section 4, always padded; see b64.h
  d["crc32"] = "hex8";       // 8 lowercase hex digits, CRC-32/ISO-HDLC
  d["max_chunk"] = (uint32_t)MAX_CHUNK;
  d["max_chunk_encoded"] = (uint32_t)B64_CHUNK_LEN;
  // max_path is the path WITHIN a volume, excluding the "/sd" or "/fs" prefix,
  // because that is the string PathSafe measures. max_volume_prefix is what a
  // caller has to add to it to get the string it actually sends.
  d["max_path"] = (uint32_t)PathSafe::MAX_PATH_LEN;
  d["max_volume_prefix"] = (uint32_t)VolPath::MAX_PREFIX_LEN;
  d["max_segment"] = (uint32_t)PathSafe::MAX_SEGMENT_LEN;
  d["max_line"] = (uint32_t)Protocol::MAX_LINE;
  d["max_file_offset"] = MAX_FILE_OFFSET;
  d["list_default_limit"] = LIST_DEFAULT_LIMIT;
  d["list_max_limit"] = LIST_MAX_LIMIT;
  d["list_max_offset"] = LIST_MAX_OFFSET;
  d["delete_max_depth"] = DELETE_MAX_DEPTH;
  d["delete_max_entries"] = DELETE_MAX_ENTRIES;
  d["max_open_files"] = MAX_OPEN_FILES;
  // Every volume, its mount state, its own limits and its free space — so a
  // client negotiates once instead of discovering each of them by failing.
  JsonArray vols = d["volumes"].to<JsonArray>();
  for (uint8_t v = 0; v < VOL_COUNT; v++) {
    fillVolume(vols.add<JsonObject>(), v, /*withUsage=*/true);
  }
  return DISPATCH_OK;
}

// Optional p.volume ("sd"/"fs") narrows it to one; with no params it reports
// every volume. One shape either way — an array — because two shapes is how a
// client ends up with two parsers.
DispatchResult actFree(JsonObjectConst p, JsonObject d, CmdError *err) {
  const char *want = p["volume"] | (const char *)nullptr;
  uint8_t only = VOL_COUNT;
  if (want != nullptr) {
    for (uint8_t v = 0; v < VOL_COUNT; v++) {
      if (strcmp(want, VOLUMES[v].name) == 0) {
        only = v;
        break;
      }
    }
    if (only == VOL_COUNT) {
      listVolumes(d);
      cmdErrorf(err, "EVOLUME", "no volume named \"%.8s\"; this device has \"sd\" (microSD) and \"fs\" (LittleFS)",
                want);
      return DISPATCH_FAIL;
    }
  }
  JsonArray vols = d["volumes"].to<JsonArray>();
  for (uint8_t v = 0; v < VOL_COUNT; v++) {
    if (only != VOL_COUNT && v != only) {
      continue;
    }
    fillVolume(vols.add<JsonObject>(), v, /*withUsage=*/true);
  }
  return DISPATCH_OK;
}

DispatchResult actList(JsonObjectConst p, JsonObject d, CmdError *err) {
  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  const char *full = r.full;

  long offsetIn = p["offset"] | 0L;
  long limitIn = p["limit"] | (long)LIST_DEFAULT_LIMIT;
  if (offsetIn < 0 || (uint32_t)offsetIn > LIST_MAX_OFFSET) {
    cmdErrorf(err, "EARGS", "p.offset must be 0..%u (readdir has no seek, so an offset is reached by skipping)",
              (unsigned)LIST_MAX_OFFSET);
    return DISPATCH_FAIL;
  }
  if (limitIn < 1) {
    limitIn = 1;
  }
  if (limitIn > (long)LIST_MAX_LIMIT) {
    limitIn = LIST_MAX_LIMIT;
  }
  uint32_t offset = (uint32_t)offsetIn;
  uint32_t limit = (uint32_t)limitIn;

  DIR *dir = opendir(full);
  if (dir == nullptr) {
    int e = errno;
    failErrno("cannot list", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }

  d["path"] = path;
  d["offset"] = offset;
  d["limit"] = limit;
  JsonArray entries = d["entries"].to<JsonArray>();

  char child[FULL_PATH_MAX];
  size_t baseLen = strlen(full);
  uint32_t skipped = 0;
  uint32_t count = 0;
  uint32_t statErrors = 0;
  bool truncated = false;
  bool budgetHit = false;
  size_t bytes = 0;

  struct dirent *de = nullptr;
  while ((de = readdir(dir)) != nullptr) {
    if (de->d_name[0] == '\0' || strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    if (skipped < offset) {
      skipped++;
      continue;
    }
    if (count >= limit) {
      truncated = true;  // there IS at least one more entry; do not guess
      break;
    }

    size_t nameLen = strlen(de->d_name);
    if (bytes + nameLen + LIST_ENTRY_OVERHEAD > LIST_BYTE_BUDGET) {
      // Second bound: the count fits but the SERIALISED page would not. Stop
      // here and let the caller come back with next_offset.
      truncated = true;
      budgetHit = true;
      break;
    }
    bytes += nameLen + LIST_ENTRY_OVERHEAD;

    JsonObject o = entries.add<JsonObject>();
    o["name"] = de->d_name;

    bool isDir = (de->d_type == DT_DIR);
    struct stat st;
    bool haveStat = false;
    if (baseLen + 1 + nameLen < sizeof(child)) {
      memcpy(child, full, baseLen);
      child[baseLen] = '/';
      memcpy(child + baseLen + 1, de->d_name, nameLen + 1);
      haveStat = (stat(child, &st) == 0);
    }
    if (haveStat) {
      isDir = S_ISDIR(st.st_mode);
      o["size"] = (uint32_t)(isDir ? 0 : st.st_size);
      o["mtime"] = (uint32_t)st.st_mtime;  // 0 == FAT held no timestamp
    } else {
      // The entry is real — readdir returned it — but we could not stat it.
      // Report it WITH the flag rather than dropping it: a listing that
      // silently omits files is worse than one that admits it is incomplete.
      statErrors++;
      o["stat_error"] = true;
    }
    o["is_dir"] = isDir;
    count++;
  }
  closedir(dir);

  d["count"] = count;
  d["truncated"] = truncated;
  d["next_offset"] = offset + count;
  if (budgetHit) {
    d["truncated_by"] = "byte_budget";
  } else if (truncated) {
    d["truncated_by"] = "limit";
  }
  if (statErrors > 0) {
    d["stat_errors"] = statErrors;
  }
  return DISPATCH_OK;
}

DispatchResult actStat(JsonObjectConst p, JsonObject d, CmdError *err) {
  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  const char *full = r.full;

  d["path"] = path;
  struct stat st;
  if (stat(full, &st) != 0) {
    int e = errno;
    if (e == ENOENT || e == ENOTDIR) {
      // "It is not there" is the ANSWER to stat, not a failure of it. read /
      // write / delete treat the same errno as an error, because for them it is.
      d["exists"] = false;
      d["errno_code"] = errnoCode(e);
      return DISPATCH_OK;
    }
    failErrno("cannot stat", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }
  bool isDir = S_ISDIR(st.st_mode);
  d["exists"] = true;
  d["is_dir"] = isDir;
  d["size"] = (uint32_t)(isDir ? 0 : st.st_size);
  d["mtime"] = (uint32_t)st.st_mtime;
  return DISPATCH_OK;
}

DispatchResult actRead(JsonObjectConst p, JsonObject d, CmdError *err) {
  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  const char *full = r.full;

  long long offsetIn = p["offset"] | 0LL;
  long long lenIn = p["len"] | (long long)MAX_CHUNK;
  if (offsetIn < 0 || offsetIn > (long long)MAX_FILE_OFFSET) {
    cmdErrorf(err, "EOFFSET", "p.offset must be 0..%u (file offsets here are 32-bit)", (unsigned)MAX_FILE_OFFSET);
    return DISPATCH_FAIL;
  }
  if (lenIn < 0) {
    cmdErrorf(err, "EARGS", "p.len must not be negative");
    return DISPATCH_FAIL;
  }
  if (lenIn > (long long)MAX_CHUNK) {
    cmdErrorf(err, "ETOOBIG", "p.len is %lld; max_chunk is %u raw bytes (see the caps action) — chunk the transfer",
              lenIn, (unsigned)MAX_CHUNK);
    return DISPATCH_FAIL;
  }

  struct stat st;
  if (stat(full, &st) != 0) {
    int e = errno;
    failErrno("cannot read", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }
  if (S_ISDIR(st.st_mode)) {
    cmdErrorf(err, "EISDIR", "\"%.64s\" is a directory; use the list action", path);
    return DISPATCH_FAIL;
  }
  uint32_t size = (uint32_t)st.st_size;
  if ((uint32_t)offsetIn > size) {
    cmdErrorf(err, "EOFFSET", "p.offset %llu is past the end of the file (size %u)", (unsigned long long)offsetIn,
              (unsigned)size);
    d["size"] = size;
    return DISPATCH_FAIL;
  }

  d["path"] = path;
  d["offset"] = (uint32_t)offsetIn;
  d["len"] = (uint32_t)lenIn;
  d["size"] = size;

  size_t got = 0;
  if (lenIn > 0 && (uint32_t)offsetIn < size) {
    FILE *fp = fopen(full, "rb");
    if (fp == nullptr) {
      int e = errno;
      failErrno("cannot open", path, e, r.vol, err);
      return DISPATCH_FAIL;
    }
    if (fseek(fp, (long)offsetIn, SEEK_SET) != 0) {
      int e = errno;
      fclose(fp);
      failErrno("cannot seek in", path, e, r.vol, err);
      return DISPATCH_FAIL;
    }
    got = fread(chunkBuf, 1, (size_t)lenIn, fp);
    bool ioErr = (ferror(fp) != 0);
    fclose(fp);
    if (ioErr) {
      cmdErrorf(err, "EIO", "read error in \"%.64s\" at offset %llu", path, (unsigned long long)offsetIn);
      d["bytes"] = (uint32_t)got;
      return DISPATCH_FAIL;
    }
  }

  size_t encLen = B64::encode(chunkBuf, got, b64Buf, sizeof(b64Buf));
  if (encLen == 0 && got > 0) {
    cmdErrorf(err, "EINTERNAL", "base64 encode of %u bytes did not fit its buffer", (unsigned)got);
    return DISPATCH_FAIL;
  }
  char hex[9];
  Crc32::toHex8(Crc32::compute(chunkBuf, got), hex);

  d["bytes"] = (uint32_t)got;
  // The (const char *) casts here and below are belt-and-braces, not the fix
  // console.cpp needed. ArduinoJson 7.4.3 only stores a string BY POINTER for
  // `const char (&)[N]` — a genuinely const array, i.e. a literal. b64Buf and
  // hex are non-const char[], which select StringAdapter<TChar[N]> and are
  // copied. The cast makes that explicit rather than dependent on constness
  // surviving future edits, since hex is a stack buffer that dies on return.
  d["data"] = (const char *)b64Buf;
  d["crc32"] = (const char *)hex;
  d["eof"] = ((uint32_t)offsetIn + (uint32_t)got >= size);
  return DISPATCH_OK;
}

DispatchResult actWrite(JsonObjectConst p, JsonObject d, CmdError *err) {
  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  const char *full = r.full;

  long long offsetIn = p["offset"] | 0LL;
  if (offsetIn < 0 || offsetIn > (long long)MAX_FILE_OFFSET) {
    cmdErrorf(err, "EOFFSET", "p.offset must be 0..%u (file offsets here are 32-bit)", (unsigned)MAX_FILE_OFFSET);
    return DISPATCH_FAIL;
  }
  uint32_t offset = (uint32_t)offsetIn;

  // `truncate` is opt-in and only means anything at offset 0. Silently ignoring
  // it elsewhere would let "truncate:true, offset:4096" read as "replace the
  // file" while doing the opposite.
  bool truncate = p["truncate"] | false;
  if (truncate && offset != 0) {
    cmdErrorf(err, "EARGS", "p.truncate is only meaningful at offset 0; you asked for offset %u", (unsigned)offset);
    return DISPATCH_FAIL;
  }

  const char *data = p["data"] | (const char *)nullptr;
  if (data == nullptr) {
    cmdErrorf(err, "EARGS", "missing p.data (base64 of the bytes to write; \"\" writes nothing)");
    return DISPATCH_FAIL;
  }
  size_t dataLen = strlen(data);
  if (dataLen > B64_CHUNK_LEN) {
    cmdErrorf(err, "ETOOBIG", "p.data is %u base64 chars; the limit is %u (max_chunk %u raw bytes)", (unsigned)dataLen,
              (unsigned)B64_CHUNK_LEN, (unsigned)MAX_CHUNK);
    return DISPATCH_FAIL;
  }
  size_t raw = 0;
  B64::Result br = B64::decode(data, dataLen, chunkBuf, sizeof(chunkBuf), &raw);
  if (br != B64::B64_OK) {
    // Nothing has touched the card at this point, and nothing will: a malformed
    // payload is rejected whole rather than written partially.
    d["b64_error"] = B64::resultName(br);
    d["b64_message"] = B64::resultMessage(br);
    cmdErrorf(err, "EB64", "%s", B64::resultMessage(br));
    return DISPATCH_FAIL;
  }

  // Decide the open mode from what is actually there, so the error can say why.
  struct stat st;
  bool exists = (stat(full, &st) == 0);
  if (exists && S_ISDIR(st.st_mode)) {
    cmdErrorf(err, "EISDIR", "\"%.64s\" is a directory", path);
    return DISPATCH_FAIL;
  }
  uint32_t existingSize = exists ? (uint32_t)st.st_size : 0u;
  if (!exists && offset != 0) {
    cmdErrorf(err, "ENOENT", "\"%.64s\" does not exist, so a write at offset %u would leave a hole; start at offset 0",
              path, (unsigned)offset);
    return DISPATCH_FAIL;
  }
  if (exists && !truncate && offset > existingSize) {
    // FatFs would happily seek past EOF and leave undefined bytes in the gap.
    // Refuse instead: writes through this API are contiguous by construction,
    // which is what makes a resumed upload verifiable with a single crc32.
    cmdErrorf(err, "EOFFSET", "p.offset %u is past the end of \"%.32s\" (size %u); writes must be contiguous",
              (unsigned)offset, path, (unsigned)existingSize);
    d["size"] = existingSize;
    return DISPATCH_FAIL;
  }

  const char *mode = (!exists || truncate) ? "wb" : "r+b";
  FILE *fp = fopen(full, mode);
  if (fp == nullptr) {
    int e = errno;
    failErrno("cannot open for writing", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }
  if (offset != 0 && fseek(fp, (long)offset, SEEK_SET) != 0) {
    int e = errno;
    fclose(fp);
    failErrno("cannot seek in", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }
  size_t written = raw > 0 ? fwrite(chunkBuf, 1, raw, fp) : 0;
  bool ioErr = (ferror(fp) != 0);
  // fclose flushes: CONFIG_FATFS_IMMEDIATE_FSYNC is off on this toolchain, so
  // the FAT is only updated here. A caller that never sees this response also
  // never sees the bytes.
  bool closeErr = (fclose(fp) != 0);

  d["path"] = path;
  d["offset"] = offset;
  d["bytes"] = (uint32_t)written;
  d["created"] = !exists;
  d["truncated"] = truncate;
  char hex[9];
  Crc32::toHex8(Crc32::compute(chunkBuf, written), hex);
  d["crc32"] = (const char *)hex;  // of what was ACTUALLY written, not of what was sent

  if (ioErr || closeErr || written != raw) {
    cmdErrorf(err, "EIO", "wrote %u of %u bytes to \"%.32s\"%s", (unsigned)written, (unsigned)raw, path,
              (written < raw) ? " — the card may be full" : "");
    return DISPATCH_FAIL;
  }
  if (stat(full, &st) == 0) {
    d["size"] = (uint32_t)st.st_size;
  }
  return DISPATCH_OK;
}

DispatchResult actMkdir(JsonObjectConst p, JsonObject d, CmdError *err) {
  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  const char *full = r.full;
  if (r.isRoot) {
    cmdErrorf(err, "EEXIST", "\"%s\" is a volume root and always exists", VOLUMES[r.vol].mount);
    return DISPATCH_FAIL;
  }
  // Single level only, deliberately: mkdir -p would have to invent intermediate
  // directories the caller never named, and on a failure half-way it would
  // leave some of them behind with nothing saying which.
  if (mkdir(full, 0777) != 0) {
    int e = errno;
    failErrno("cannot mkdir", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }
  d["path"] = path;
  d["created"] = true;
  return DISPATCH_OK;
}

// ---- recursive delete ----------------------------------------------------

enum DelErr : uint8_t { DEL_OK = 0, DEL_DEPTH, DEL_LIMIT, DEL_PATHLEN, DEL_OPEN, DEL_IO };

struct DelState {
  uint32_t files = 0;
  uint32_t dirs = 0;
  int lastErrno = 0;
  char failPath[FULL_PATH_MAX] = {0};
};

void noteFail(DelState *s, const char *full) {
  if (s->failPath[0] == '\0') {
    snprintf(s->failPath, sizeof(s->failPath), "%s", full);
  }
}

// Deletes the directory at `full` and everything under it. `full` is a
// mutable buffer holding the FULL VFS path; it is appended to and truncated in
// place so that recursion costs one 201-byte name buffer per level rather than
// a whole path buffer.
//
// THE OPEN/READ-ONE/CLOSE/DELETE LOOP IS DELIBERATE. FatFs invalidates a
// directory object when the directory it is scanning is modified, so unlinking
// during an f_readdir walk is undefined. Instead each pass opens the directory,
// takes the FIRST entry, closes it, and then deletes that entry — so no handle
// is ever open across a mutation. It is still O(n) overall, because every pass
// removes the entry it just read and the next pass's first entry is the one
// after it.
DelErr removeTree(char *full, size_t fullCap, uint8_t depth, DelState *s) {
  if (depth > DELETE_MAX_DEPTH) {
    noteFail(s, full);
    return DEL_DEPTH;
  }
  size_t baseLen = strlen(full);

  for (;;) {
    if (s->files + s->dirs >= DELETE_MAX_ENTRIES) {
      noteFail(s, full);
      return DEL_LIMIT;
    }

    DIR *dir = opendir(full);
    if (dir == nullptr) {
      s->lastErrno = errno;
      noteFail(s, full);
      return DEL_OPEN;
    }
    char name[PathSafe::MAX_SEGMENT_LEN + 1];
    name[0] = '\0';
    bool isDir = false;
    bool nameTooLong = false;
    struct dirent *de = nullptr;
    while ((de = readdir(dir)) != nullptr) {
      if (de->d_name[0] == '\0' || strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
        continue;
      }
      size_t nl = strlen(de->d_name);
      if (nl >= sizeof(name)) {
        nameTooLong = true;
        break;
      }
      memcpy(name, de->d_name, nl + 1);
      isDir = (de->d_type == DT_DIR);
      break;
    }
    closedir(dir);  // closed BEFORE any mutation — see the note above

    if (nameTooLong) {
      noteFail(s, full);
      return DEL_PATHLEN;
    }
    if (name[0] == '\0') {
      break;  // empty now
    }

    size_t nameLen = strlen(name);
    if (baseLen + 1 + nameLen >= fullCap) {
      noteFail(s, full);
      return DEL_PATHLEN;
    }
    full[baseLen] = '/';
    memcpy(full + baseLen + 1, name, nameLen + 1);

    if (!isDir) {
      // d_type can be DT_UNKNOWN on a VFS that does not fill it in, and calling
      // unlink() on a directory because we assumed d_type was meaningful would
      // leave the tree half-deleted with a misleading errno. Confirm with stat.
      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        isDir = true;
      }
    }

    if (isDir) {
      DelErr e = removeTree(full, fullCap, (uint8_t)(depth + 1), s);
      if (e != DEL_OK) {
        full[baseLen] = '\0';
        return e;
      }
    } else {
      if (unlink(full) != 0) {
        s->lastErrno = errno;
        noteFail(s, full);
        full[baseLen] = '\0';
        return DEL_IO;
      }
      s->files++;
    }
    full[baseLen] = '\0';
  }

  if (rmdir(full) != 0) {
    s->lastErrno = errno;
    noteFail(s, full);
    return DEL_IO;
  }
  s->dirs++;
  return DEL_OK;
}

DispatchResult actDelete(JsonObjectConst p, JsonObject d, CmdError *err) {
  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  // MUTABLE, unlike the other actions: removeTree() appends to and truncates
  // this buffer in place, which is what keeps the recursion at one 201-byte
  // name buffer per level instead of a whole path buffer.
  char *full = r.full;
  bool recursive = p["recursive"] | false;

  d["path"] = path;
  d["volume"] = VOLUMES[r.vol].name;
  d["recursive"] = recursive;

  if (r.isRoot) {
    // Deleting a volume root recursively is "erase the card" / "erase the
    // LittleFS partition". If that is ever wanted it gets its own named action
    // with its own confirmation, not a flag on this one — and for /fs it
    // already has one: `storage.format`, at AUTH_PHYSICAL.
    cmdErrorf(err, "EARGS", "refusing to delete the volume root \"%s\"; name a file or directory inside it",
              VOLUMES[r.vol].mount);
    return DISPATCH_FAIL;
  }

  struct stat st;
  if (stat(full, &st) != 0) {
    int e = errno;
    failErrno("cannot delete", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }

  if (!S_ISDIR(st.st_mode)) {
    if (unlink(full) != 0) {
      int e = errno;
      failErrno("cannot delete", path, e, r.vol, err);
      return DISPATCH_FAIL;
    }
    d["files"] = 1;
    d["dirs"] = 0;
    return DISPATCH_OK;
  }

  if (!recursive) {
    if (rmdir(full) != 0) {
      int e = errno;
      failErrno("cannot delete", path, e, r.vol, err);
      return DISPATCH_FAIL;
    }
    d["files"] = 0;
    d["dirs"] = 1;
    return DISPATCH_OK;
  }

  DelState s;
  DelErr e = removeTree(full, sizeof(r.full), 1, &s);
  // Counts go into `d` on BOTH paths. A bounded recursive delete that gives up
  // half-way has already changed the card, and a caller that cannot see how much
  // was removed cannot safely retry. `d` survives an error (ARCHITECTURE.md §2).
  d["files"] = s.files;
  d["dirs"] = s.dirs;
  if (e == DEL_OK) {
    return DISPATCH_OK;
  }

  d["complete"] = false;
  if (s.failPath[0] != '\0') {
    // Report the VFS path in the CALLER'S terms — which, because the volume
    // prefix and the mountpoint are the same string, is the VFS path itself.
    // Written as the volume prefix plus the remainder anyway, so that the day
    // those two stop being identical this line does not quietly start lying.
    char shown[FULL_PATH_MAX];
    snprintf(shown, sizeof(shown), "%s%s", VOLUMES[r.vol].mount, s.failPath + VOLUMES[r.vol].mountLen);
    d["failed_at"] = (const char *)shown;
  }
  switch (e) {
    case DEL_DEPTH:
      cmdErrorf(err, "EDEPTH", "recursive delete stopped at depth %u (removed %u files, %u dirs so far)",
                (unsigned)DELETE_MAX_DEPTH, (unsigned)s.files, (unsigned)s.dirs);
      break;
    case DEL_LIMIT:
      cmdErrorf(err, "ELIMIT", "recursive delete stopped at the %u-entry bound (removed %u files, %u dirs)",
                (unsigned)DELETE_MAX_ENTRIES, (unsigned)s.files, (unsigned)s.dirs);
      break;
    case DEL_PATHLEN:
      cmdErrorf(err, "EPATHLEN", "a nested path exceeds the %u-byte path limit; it cannot be addressed by this API",
                (unsigned)PathSafe::MAX_PATH_LEN);
      break;
    case DEL_OPEN:
      cmdErrorf(err, errnoCode(s.lastErrno), "cannot open a directory part-way through the delete: %s (errno %d)",
                errnoAdvice(s.lastErrno, r.vol), s.lastErrno);
      break;
    default:
      cmdErrorf(err, errnoCode(s.lastErrno), "delete failed part-way through: %s (errno %d)",
                errnoAdvice(s.lastErrno, r.vol), s.lastErrno);
      break;
  }
  return DISPATCH_FAIL;
}

DispatchResult actVerify(const CmdContext &ctx, JsonObjectConst p, JsonObject d, CmdError *err) {
  if (p["cancel"] | false) {
    if (!verify.active) {
      cmdErrorf(err, "ENOJOB", "no verify job is running");
      return DISPATCH_FAIL;
    }
    uint32_t job = verify.id;
    finishVerify("ECANCELLED", "cancelled by request");
    d["cancelled"] = true;
    d["job"] = job;
    return DISPATCH_OK;
  }

  if (verify.active) {
    d["job"] = verify.id;
    d["bytes"] = verify.bytes;
    d["size"] = verify.size;
    cmdErrorf(err, "EBUSY", "verify job %u is already running (%u of %u bytes); send p:{cancel:true} to stop it",
              (unsigned)verify.id, (unsigned)verify.bytes, (unsigned)verify.size);
    return DISPATCH_FAIL;
  }

  Resolved r;
  const char *path = p["path"] | (const char *)nullptr;
  if (!resolvePath(path, &r, d, err)) {
    return DISPATCH_FAIL;
  }
  const char *full = r.full;

  struct stat st;
  if (stat(full, &st) != 0) {
    int e = errno;
    failErrno("cannot verify", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }
  if (S_ISDIR(st.st_mode)) {
    cmdErrorf(err, "EISDIR", "\"%.64s\" is a directory; verify hashes one file", path);
    return DISPATCH_FAIL;
  }

  FILE *fp = fopen(full, "rb");
  if (fp == nullptr) {
    int e = errno;
    failErrno("cannot open", path, e, r.vol, err);
    return DISPATCH_FAIL;
  }

  verify.fp = fp;
  verify.active = true;
  verify.id = ++verifyJobCounter;
  verify.reqId = ctx.reqId;
  verify.crc = Crc32::INIT;
  verify.bytes = 0;
  verify.size = (uint32_t)st.st_size;
  verify.lastProgressMs = millis();
  verify.haveResult = false;
  verify.resultCode = nullptr;
  verify.resultMsg[0] = '\0';
  verify.vol = r.vol;
  snprintf(verify.path, sizeof(verify.path), "%s", path);
  Activity::begin("storage", "verify");  // out-of-band progress; see activity.h

  d["job"] = verify.id;
  d["path"] = path;
  d["volume"] = VOLUMES[r.vol].name;
  d["size"] = verify.size;
  // ACCEPTED, not OK: hashing a multi-megabyte file inline would hold the
  // cooperative scheduler for seconds. The result arrives as
  // storage.verify.done, correlated by this request's id.
  return DISPATCH_ACCEPTED;
}

// ===========================================================================
// Status and dispatch
// ===========================================================================

// ---- format: the ONLY thing in this image that erases a filesystem --------
//
// /fs ONLY, and deliberately. Formatting the microSD is NOT offered: the card
// must stay FAT32 for this board (see ../CLAUDE.md "microSD — resolved"), the
// only formatter available on-device would produce whatever esp_vfs_fat chose,
// and `mkfs.vfat -F 32 -s 64` on a PC is the documented, verified path. An
// action that could quietly produce a differently-formatted card is one more
// way to arrive back at the exFAT afternoon.
DispatchResult actFormat(JsonObjectConst p, JsonObject d, CmdError *err) {
  const char *volume = p["volume"] | (const char *)nullptr;
  bool confirm = p["confirm"] | false;

  if (volume == nullptr) {
    listVolumes(d);
    cmdErrorf(err, "EARGS", "format needs p.volume; only \"fs\" can be formatted by this device");
    return DISPATCH_FAIL;
  }
  if (strcmp(volume, "sd") == 0) {
    cmdErrorf(err, "EUNSUPPORTED",
              "the microSD is not formatted by this device; it must stay FAT32 — use mkfs.vfat -F 32 -s 64 on a PC");
    return DISPATCH_FAIL;
  }
  if (strcmp(volume, "fs") != 0) {
    listVolumes(d);
    cmdErrorf(err, "EVOLUME", "no volume named \"%.8s\"; only \"fs\" can be formatted", volume);
    return DISPATCH_FAIL;
  }
  if (!confirm) {
    // A second, explicit word. `p:{volume:"fs"}` alone is one typo away from
    // being sent by someone who meant `free`, and this erases 7.75 MB of web
    // assets and macros with no undo.
    d["volume"] = "fs";
    d["would_erase"] = Fs::partitionSize();
    cmdErrorf(err, "ECONFIRM",
              "this ERASES the whole /fs volume and cannot be undone; resend with p:{volume:\"fs\",confirm:true}");
    return DISPATCH_FAIL;
  }
  if (verify.active) {
    // The verify job holds an open FILE* which may live on this very volume.
    // Refusing is better than cancelling somebody else's job as a side effect
    // (the same stance actVerify takes about a second verify).
    d["job"] = verify.id;
    d["verify_path"] = (const char *)verify.path;
    cmdErrorf(err, "EBUSY", "verify job %u is running and holds an open file; cancel it first", (unsigned)verify.id);
    return DISPATCH_FAIL;
  }

  uint64_t beforeTotal = 0, beforeUsed = 0;
  bool hadInfo = Fs::info(&beforeTotal, &beforeUsed);

  const char *code = "EFORMAT";
  char msg[160];
  msg[0] = '\0';
  bool ok = Fs::format(&code, msg, sizeof(msg));

  d["volume"] = "fs";
  d["mounted"] = Fs::mounted();
  d["stage"] = Fs::stageName();
  if (hadInfo) {
    d["used_before"] = beforeUsed;
  }
  uint64_t total = 0, used = 0;
  if (Fs::info(&total, &used)) {
    d["total"] = total;
    d["used"] = used;
    d["free"] = (total >= used) ? (total - used) : 0;
  }
  if (!ok) {
    cmdErrorf(err, code, "%s", msg);
    return DISPATCH_FAIL;
  }
  d["formatted"] = true;
  d["msg"] = (const char *)msg;  // cast: char[] would be stored by pointer
  return DISPATCH_OK;
}

void fillStatus(JsonObject d) {
  // Per-volume, WITHOUT usage: status() runs on every `modules` command, and
  // SD_MMC.totalBytes() is f_getfree(), which walks the whole FAT if the FAT32
  // FSInfo free-cluster count is stale. That belongs behind the explicit `free`
  // and `caps` actions, where the caller asked for it.
  JsonArray vols = d["volumes"].to<JsonArray>();
  for (uint8_t v = 0; v < VOL_COUNT; v++) {
    fillVolume(vols.add<JsonObject>(), v, /*withUsage=*/false);
  }
  // The SD diagnosis in full, and the LittleFS one, because the two-stage
  // failure story is the whole reason this module reports a `stage` at all.
  d["sd_detail"] = (const char *)mountDetail;
  if (lastStageErr != 0) {
    d["sd_stage_err"] = lastStageErr;
  }
  d["fs_detail"] = Fs::detail();
  if (Fs::lastErr() != 0) {
    d["fs_stage_err"] = Fs::lastErr();
  }
  d["verify_active"] = verify.active;
  d["verify_job"] = verify.id;
  if (verify.active) {
    d["verify_path"] = (const char *)verify.path;
    d["verify_bytes"] = verify.bytes;
    d["verify_size"] = verify.size;
  } else if (verify.haveResult) {
    d["verify_last_ok"] = verify.resultOk;
    if (verify.resultOk) {
      char hex[9];
      Crc32::toHex8(verify.resultCrc, hex);
      d["verify_last_crc32"] = (const char *)hex;
    } else {
      d["verify_last_code"] = verify.resultCode ? verify.resultCode : "EFAIL";
    }
  }
  d["open_files_reserved"] = MAX_OPEN_FILES;
}

DispatchResult storageDispatch(const CmdContext &ctx, const char *act, JsonObjectConst p, JsonObject d,
                               CmdError *err) {
  // Read-only tier. Everything here still needs AUTH_TOKEN — a directory
  // listing of someone's card is not public information — but the check is
  // Registry::dispatch()'s now, from the table in modauth.h. See the Auth note
  // above.
  if (strcmp(act, "caps") == 0) {
    return actCaps(d);
  }
  if (strcmp(act, "free") == 0) {
    return actFree(p, d, err);
  }
  if (strcmp(act, "list") == 0) {
    return actList(p, d, err);
  }
  if (strcmp(act, "stat") == 0) {
    return actStat(p, d, err);
  }
  if (strcmp(act, "read") == 0) {
    return actRead(p, d, err);
  }
  if (strcmp(act, "verify") == 0) {
    return actVerify(ctx, p, d, err);
  }

  // Mutating tier.
  if (strcmp(act, "write") == 0) {
    return actWrite(p, d, err);
  }
  if (strcmp(act, "mkdir") == 0) {
    return actMkdir(p, d, err);
  }
  if (strcmp(act, "delete") == 0) {
    return actDelete(p, d, err);
  }

  // Destructive tier — ONE action, and it is above the mutating tier rather
  // than in it: AUTH_PHYSICAL in modauth.h, enforced before this runs. The
  // second gate it still applies for itself is p.confirm, which is a
  // confirmation, not an authorisation.
  if (strcmp(act, "format") == 0) {
    return actFormat(p, d, err);
  }

  if (strcmp(act, "status") == 0) {
    fillStatus(d);
    return DISPATCH_OK;
  }

  cmdErrorf(err, "EUNKNOWN",
            "unknown action for module 'storage': \"%.16s\" "
            "(caps/free/list/stat/read/write/mkdir/delete/verify/format/status)",
            act);
  return DISPATCH_FAIL;
}

void storageStatus(JsonObject d) { fillStatus(d); }

// Static .rodata. This is what lets the web UI render storage's controls with
// no module-specific front-end code (ARCHITECTURE.md section 2) — and `caps` is
// what lets it size its chunks without guessing.
//
// Read off the handlers above, not off the prose hints these replace. Three of
// those hints were WRONG, in the direction that costs a caller a round trip:
// `read` advertised "path,offset,len" and `write` "path,offset,data" as though
// offset and len were required, when both default (offset 0, len max_chunk);
// and `verify`'s "path|cancel:true" implied a choice the dispatch does not
// enforce — cancel:true is checked FIRST and ignores path entirely.
const ModuleParam PATH_ONLY[] = {
    ModParam::str("path", true,
                  "volume-prefixed path, e.g. \"/sd\", \"/sd/logs/run.txt\" or \"/fs/www/index.html.gz\". "
                  "\"/sd\" and \"/fs\" are the volume roots; a trailing '/' is refused."),
};

const ModuleParam LIST_PARAMS[] = {
    ModParam::str("path", true,
                  "volume-prefixed directory path; \"/sd\" and \"/fs\" are the two volume roots."),
    ModParam::num("offset", false, "entries to skip; default 0. readdir has no seek, so this is reached by skipping.", 0,
                  (int32_t)LIST_MAX_OFFSET),
    ModParam::num("limit", false, "max entries in this page; default 32. Out-of-range values are CLAMPED, not rejected.",
                  1, (int32_t)LIST_MAX_LIMIT),
};

const ModuleParam READ_PARAMS[] = {
    ModParam::str("path", true, "volume-prefixed file path, e.g. \"/sd/logs/run.txt\" or \"/fs/macros/a.json\"."),
    ModParam::num("offset", false, "byte offset to read from; default 0. File offsets here are 32-bit.", 0,
                  (int32_t)MAX_FILE_OFFSET),
    ModParam::num("len", false, "raw bytes to read; default and maximum are max_chunk (see the caps action). Larger is rejected.",
                  0, (int32_t)MAX_CHUNK),
};

const ModuleParam WRITE_PARAMS[] = {
    ModParam::str("path", true, "volume-prefixed file path, e.g. \"/sd/logs/run.txt\" or \"/fs/macros/a.json\"."),
    ModParam::str("data", true, "base64 of the bytes to write (padded, RFC 4648 §4). \"\" writes nothing."),
    ModParam::num("offset", false, "byte offset to write at; default 0. Writes must be contiguous — past EOF is refused.", 0,
                  (int32_t)MAX_FILE_OFFSET),
    ModParam::flag("truncate", false, "replace the file rather than overwrite in place. Only valid at offset 0; refused elsewhere."),
};

const ModuleParam DELETE_PARAMS[] = {
    ModParam::str("path", true, "volume-prefixed path. A volume root (\"/sd\", \"/fs\") is refused."),
    ModParam::flag("recursive", false,
                   "delete a non-empty directory tree, bounded to 8 levels and 2000 entries. Without it, only a file "
                   "or an EMPTY directory is removed."),
};

const ModuleParam VERIFY_PARAMS[] = {
    // BOTH optional, and that is the honest description: actVerify checks
    // cancel FIRST and returns without looking at path, so neither is
    // unconditionally required and there is no "oneOf" in the dispatch.
    ModParam::str("path", false,
                  "volume-prefixed file path to hash. Required unless cancel is true, which is handled first."),
    ModParam::flag("cancel", false, "cancel the RUNNING verify job instead of starting one; path is then ignored."),
};

const ModuleParam FREE_PARAMS[] = {
    ModParam::str("volume", false, "\"sd\" or \"fs\"; omit to report every volume. Names the volume, not a path."),
};

const ModuleParam FORMAT_PARAMS[] = {
    ModParam::str("volume", true, "\"fs\" — the LittleFS partition. \"sd\" is refused: the card must stay FAT32."),
    ModParam::flag("confirm", true, "required second word. Without it nothing is erased and ECONFIRM is returned."),
};

constexpr ModuleAction STORAGE_ACTIONS[] = {
    {"caps",
     "transfer limits (max_chunk, max_path, crc32 format, listing/delete bounds) plus every volume, its mount state, "
     "its own limits and its free space",
     nullptr, 0, ModAuth::requiredFor("storage", "caps")},
    {"free", "total/used/free per volume (walks the FAT for /sd if the FAT32 free count is stale)",
     MOD_PARAMS(FREE_PARAMS), ModAuth::requiredFor("storage", "free")},
    {"list", "one PAGE of a directory; returns truncated + next_offset", MOD_PARAMS(LIST_PARAMS),
     ModAuth::requiredFor("storage", "list")},
    {"stat", "exists / is_dir / size / mtime for one path (a missing path is ok:true with exists:false)",
     MOD_PARAMS(PATH_ONLY), ModAuth::requiredFor("storage", "stat")},
    {"read", "read one chunk as base64 with a crc32; the caller drives offset, so it resumes by construction",
     MOD_PARAMS(READ_PARAMS), ModAuth::requiredFor("storage", "read")},
    {"write", "write one base64 chunk at an offset; returns the crc32 of what was written", MOD_PARAMS(WRITE_PARAMS),
     ModAuth::requiredFor("storage", "write")},
    {"mkdir", "create one directory (not -p: intermediate directories are not invented)", MOD_PARAMS(PATH_ONLY),
     ModAuth::requiredFor("storage", "mkdir")},
    {"delete", "delete a file, an EMPTY directory, or a bounded tree with recursive:true", MOD_PARAMS(DELETE_PARAMS),
     ModAuth::requiredFor("storage", "delete")},
    {"verify", "crc32 a whole file (queued; progress + storage.verify.done events). cancel:true stops it",
     MOD_PARAMS(VERIFY_PARAMS), ModAuth::requiredFor("storage", "verify")},
    // The one PHYSICAL row in this module, and it was PHYSICAL before the table
    // existed. The help still says so in English because it is the one action
    // whose refusal an operator has to be able to predict from the console.
    {"format",
     "ERASE the whole /fs volume and remount it empty. USB console only (auth >= physical) and needs confirm:true. "
     "The microSD is never formatted here",
     MOD_PARAMS(FORMAT_PARAMS), ModAuth::requiredFor("storage", "format")},
    {"status", "per-volume mount state, the failing bring-up stage for each, and the current verify job", nullptr, 0,
     ModAuth::requiredFor("storage", "status")},
};
static_assert(ModAuth::allGated(STORAGE_ACTIONS, sizeof(STORAGE_ACTIONS) / sizeof(STORAGE_ACTIONS[0])),
              "storage: an action has no declared auth level");
static_assert(ModAuth::isModuleListed("storage"), "storage has no row in ModAuth::MODULES");
static_assert(ModAuth::requiredFor("storage", "format") == ModAuth::PHYSICAL,
              "storage.format must stay AUTH_PHYSICAL — it erases a whole filesystem");

const ModuleDescriptor STORAGE_MODULE = {
    .id = "storage",
    .name = "Files (microSD + LittleFS)",
    .category = "storage",
    // SHARED, not exclusive: several readers of the card can coexist. `msc`
    // takes RES_SD EXCLUSIVE when it hands the raw block device to the host PC,
    // and that is what locks this module out — by arbitration, with neither
    // module naming the other. See claims.h.
    //
    // STILL ONLY RES_SD, even though this module now also serves /fs. LittleFS
    // is not a contended resource: it is mounted at boot by fsmount.cpp and
    // nothing else in the image wants it exclusively, so claiming it would
    // invent a conflict that does not exist. The consequence — that `storage`
    // holds RES_SD even when there is no card and it is serving /fs alone — is
    // written up above storageEnable().
    .claims = Claims::claim(Claims::RES_SD, Claims::CLAIM_SHARED),
    // Off out of the box. Mounting the card costs time and holds RES_SD, and
    // exposing the user's files is opt-in.
    .defaultEnabled = false,
    // Nothing here binds at boot: SDMMC is a runtime peripheral, and both
    // enable() and disable() really do mount and unmount. /fs is mounted at
    // boot by Fs::begin() and is unaffected by either.
    .bootTimeBinding = false,
    .essential = false,
    .minAuth = ModAuth::moduleMinimum("storage"),
    .enable = storageEnable,
    .disable = storageDisable,
    .dispatch = storageDispatch,
    .status = storageStatus,
    .actions = STORAGE_ACTIONS,
    .actionCount = (uint8_t)(sizeof(STORAGE_ACTIONS) / sizeof(STORAGE_ACTIONS[0])),
    // The verify pump. Registered with the scheduler and gated on `enabled` by
    // the registry; the module neither registers nor gates it. Each pass is
    // bounded by BOTH a byte budget and a time budget.
    .tick = storageTick,
    .tickIntervalMs = VERIFY_TICK_MS,
};

}  // namespace

const ModuleDescriptor *storageModuleDescriptor() { return &STORAGE_MODULE; }
