#include "otaupload.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "activity.h"
#include "bus.h"
#include "otahealth.h"

namespace {

// ---- threading -----------------------------------------------------------
//
// run() executes ENTIRELY ON THE CALLER'S TASK, which is the esp_http_server
// task (mod_http.cpp), for the whole duration of the transfer. Two rules follow
// and both are enforced by the caller, not here:
//
//   * NO REGISTRY LOCK. This file never touches the registry, and the handler
//     that calls it does not hold the lock across the transfer. A ten-second
//     write holding the lock would stall every module tick on the loop task.
//   * NOT THE LOOP TASK. Nothing cooperative can run while this blocks.
//
// fillStatus() can run on EITHER task (a `ota` over CDC, or over /api/cmd). It
// only reads, and everything it reads is either a word-sized scalar — naturally
// atomic on this core — or one of the fixed char buffers in `st_`, which are
// memset to zero at the start of every run and only written once, at the end.
// A concurrent reader therefore always finds a NUL inside the buffer; the worst
// case is one status response showing the previous upload's build string. That
// is the same trade otahealth.cpp and activity.h make, for the same reason: a
// mutex to protect a progress report is not worth a FreeRTOS dependency.
//
// busy_ is the one thing that must be exact, because it is what makes "one
// upload at a time" true, so it is a compare-exchange rather than a flag.

std::atomic<bool> busy_{false};

enum Phase : uint8_t {
  PH_IDLE = 0,
  PH_RUNNING = 1,
  PH_DONE_OK = 2,
  PH_DONE_FAIL = 3,
};

struct StatusState {
  Phase phase;
  uint32_t startedMs;
  uint32_t finishedMs;
  uint32_t declared;
  uint32_t written;
  bool select;
  bool selected;
  bool shaChecked;
  bool shaOk;
  const char *code;  // static string only
  char target[17];
  char msg[176];
  char build[48];
};

StatusState st_ = {};

// ---- activity.h reporting ------------------------------------------------
//
// Same guard otahealth.cpp uses, and now for a sharper reason: this is the
// FIRST activity.h producer that runs on a task other than the loop task.
// activity.h has no lock (backlog C6) and one producer at a time by convention,
// so an upload starting while `storage.verify` or the OTA health check owns the
// slot can only produce a mixed label for one frame — never a crash, never a
// leak. stillOurs() stops us stamping a percentage onto somebody else's label.
constexpr char ACT_WHO[] = "ota";
constexpr char ACT_VERB[] = "upload";
bool actOwned_ = false;

bool stillOurs() {
  if (!actOwned_) {
    return false;
  }
  Activity::Snapshot a;
  if (!Activity::snapshot(a) || a.state != Activity::RUNNING) {
    return false;
  }
  return strcmp(a.who, ACT_WHO) == 0 && strcmp(a.verb, ACT_VERB) == 0;
}

void actBegin() {
  Activity::begin(ACT_WHO, ACT_VERB);
  actOwned_ = Activity::subscribed();
}

void actEnd(bool ok) {
  if (stillOurs()) {
    Activity::end(ok);
  }
  actOwned_ = false;
}

// ---- bus events ----------------------------------------------------------
//
// DELIBERATELY FEW. The WebSocket sink cannot drain while an HTTP handler is
// running — esp_http_server services its control socket from the same task —
// so every event emitted during a transfer sits in mod_http.cpp's 6-slot ring
// until this handler returns, and the ring drops what does not fit (counted as
// events_dropped). Two progress points, not twenty. The CDC sink writes
// synchronously and sees all of them regardless, which is what matters on the
// bench; the uploading client gets the whole outcome in the HTTP response body
// either way.
constexpr uint8_t PROGRESS_EVENT_POINTS = 2;
constexpr uint8_t PROGRESS_EVENT_PCT[PROGRESS_EVENT_POINTS] = {33, 66};

struct ProgressEvt {
  const char *target;
  uint32_t written;
  uint32_t declared;
  uint8_t pct;
};

void fillProgress(JsonObject d, void *ctx) {
  const ProgressEvt *e = (const ProgressEvt *)ctx;
  if (e == nullptr) {
    return;
  }
  d["target"] = e->target;
  d["written"] = e->written;
  d["len"] = e->declared;
  d["pct"] = e->pct;
}

struct BeginEvt {
  const char *target;
  uint32_t offset;
  uint32_t size;
  uint32_t declared;
  bool sha;
  bool select;
};

void fillBegin(JsonObject d, void *ctx) {
  const BeginEvt *e = (const BeginEvt *)ctx;
  if (e == nullptr) {
    return;
  }
  d["target"] = e->target;
  d["offset"] = e->offset;
  d["slot_size"] = e->size;
  d["len"] = e->declared;
  d["sha256_declared"] = e->sha;
  d["select"] = e->select;
}

void fillDone(JsonObject d, void *ctx) {
  const OtaUpload::Report *r = (const OtaUpload::Report *)ctx;
  if (r == nullptr) {
    return;
  }
  // NO SECRET REACHES HERE, and there is none to reach it: an image, its size,
  // its digest and its build string are all public facts about the firmware.
  d["ok"] = r->ok;
  d["target"] = (const char *)r->target;
  d["len"] = r->declared;
  d["written"] = r->written;
  d["elapsed_ms"] = r->elapsedMs;
  d["sha256"] = (const char *)r->sha256;
  if (r->shaChecked) {
    d["sha256_ok"] = r->shaOk;
  }
  if (r->ok) {
    d["build"] = (const char *)r->build;
    d["idf_version"] = (const char *)r->idfVersion;
  } else {
    d["code"] = r->code != nullptr ? r->code : "EFAIL";
    d["msg"] = (const char *)r->msg;
  }
  d["selected"] = r->selected;
  d["boot"] = (const char *)r->bootPartition;
  d["reboot_required"] = r->rebootRequired;
}

// ---- helpers -------------------------------------------------------------

void toHex(const uint8_t *in, size_t n, char *out) {
  // Not named HEX: Print.h defines that as a macro (16) and the shadowing
  // failure is an unreadable "expected unqualified-id".
  static const char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = DIGITS[in[i] >> 4];
    out[i * 2 + 1] = DIGITS[in[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

// Sets the failure fields on the report AND on the status snapshot in one
// place, so a code that reaches the caller cannot fail to reach `ota`.
void fail(OtaUpload::Report &rep, const char *code, uint16_t status, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

void fail(OtaUpload::Report &rep, const char *code, uint16_t status, const char *fmt, ...) {
  rep.ok = false;
  rep.code = code;
  rep.httpStatus = status;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(rep.msg, sizeof(rep.msg), fmt, ap);
  va_end(ap);
}

// esp_ota_begin()'s documented failures, each turned into a code a caller can
// act on. EPENDING is the one that will actually happen on this device: IDF
// refuses to start an OTA while the running image is still PENDING_VERIFY, and
// that is exactly the state the FIRST 30 seconds after an OTA boot are in.
const char *beginCode(esp_err_t e) {
  switch (e) {
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
      return "EPENDING";
    case ESP_ERR_OTA_PARTITION_CONFLICT:
      return "ECONFLICT";
    case ESP_ERR_NO_MEM:
      return "ENOMEM";
    case ESP_ERR_INVALID_SIZE:
      return "ETOOBIG";
    case ESP_ERR_OTA_SELECT_INFO_INVALID:
      return "EOTADATA";
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_ARG:
      return "ENOSLOT";
    default:
      return "EOTABEGIN";
  }
}

const char *beginAdvice(esp_err_t e) {
  switch (e) {
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
      return "this image is still PENDING_VERIFY, so IDF refuses to start another OTA. Confirm it first "
             "(`ota confirm` over USB, or wait for the health check) and retry";
    case ESP_ERR_OTA_PARTITION_CONFLICT:
      return "the target slot is the one currently running; it cannot be updated in place";
    case ESP_ERR_NO_MEM:
      return "not enough heap to start an OTA; disable a module and retry";
    case ESP_ERR_OTA_SELECT_INFO_INVALID:
      return "the otadata partition holds invalid data";
    default:
      return "esp_ota_begin() refused";
  }
}

}  // namespace

namespace OtaUpload {

bool busy() { return busy_.load(std::memory_order_acquire); }

bool run(const Params &p, ReadFn read, void *ctx, Report &rep) {
  rep = Report{};

  // ---- one at a time -----------------------------------------------------
  bool expected = false;
  if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    // NOT queued: a second image arriving mid-transfer would have to erase the
    // slot the first one is halfway through writing.
    fail(rep, "EBUSY", 409, "an OTA upload is already running (%u of %u bytes); wait for it to finish",
         (unsigned)st_.written, (unsigned)st_.declared);
    return false;
  }

  const uint32_t startMs = millis();
  memset(&st_, 0, sizeof(st_));
  st_.phase = PH_RUNNING;
  st_.startedMs = startMs;
  st_.declared = p.declaredLen;
  st_.select = p.select;

  // A single exit path for everything below, so busy_ cannot be left set and
  // the buffer cannot be leaked by a return added later.
  esp_ota_handle_t handle = 0;
  bool handleOpen = false;
  uint8_t *buf = nullptr;
  bool ok = false;

  do {
    // ---- the target slot -------------------------------------------------
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == nullptr) {
      fail(rep, "ENOSLOT", 500,
           "there is no inactive OTA slot to write to; this image's partition table has no second app partition");
      break;
    }
    snprintf(rep.target, sizeof(rep.target), "%s", target->label);
    snprintf(st_.target, sizeof(st_.target), "%s", target->label);
    rep.targetOffset = target->address;
    rep.targetSize = target->size;
    rep.declared = p.declaredLen;

    // ---- the declared length, checked BEFORE anything is erased ----------
    if (p.declaredLen == 0) {
      fail(rep, "EARGS", 400, "the image size must be declared up front and must not be zero");
      break;
    }
    if (p.declaredLen < MIN_IMAGE_BYTES) {
      fail(rep, "ETOOSMALL", 400, "%u bytes cannot be an ESP32-S3 app image (the minimum accepted here is %u)",
           (unsigned)p.declaredLen, (unsigned)MIN_IMAGE_BYTES);
      break;
    }
    if (p.declaredLen > target->size) {
      // THE HARD CAP. esp_ota_write would refuse out-of-bounds anyway, but only
      // after erasing the slot and taking the whole transfer first.
      fail(rep, "ETOOBIG", 413, "%u bytes will not fit slot '%s' (%u bytes)", (unsigned)p.declaredLen, target->label,
           (unsigned)target->size);
      break;
    }

    // ---- open the slot ---------------------------------------------------
    //
    // The declared size is passed rather than OTA_SIZE_UNKNOWN so IDF erases
    // only the sectors this image needs — seconds instead of tens of them on a
    // 4 MB slot, and far less wear.
    esp_err_t e = esp_ota_begin(target, p.declaredLen, &handle);
    if (e != ESP_OK) {
      fail(rep, beginCode(e), (e == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) ? 409 : 500, "esp_ota_begin('%s') failed: %s — %s",
           target->label, esp_err_to_name(e), beginAdvice(e));
      break;
    }
    handleOpen = true;

    buf = (uint8_t *)malloc(BUF_SIZE);
    if (buf == nullptr) {
      fail(rep, "ENOMEM", 500, "out of heap for the %u-byte transfer buffer", (unsigned)BUF_SIZE);
      break;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);  // 0 == SHA-256, not SHA-224

    actBegin();
    {
      BeginEvt ev = {target->label, target->address, target->size, p.declaredLen, p.haveSha, p.select};
      Bus::emit("ota.upload.begin", fillBegin, &ev);
    }

    // ---- the transfer ----------------------------------------------------
    uint32_t written = 0;
    uint32_t lastDataMs = millis();
    uint8_t nextPoint = 0;
    const char *failCode = nullptr;

    while (written < p.declaredLen) {
      uint32_t now = millis();
      if ((uint32_t)(now - startMs) >= TOTAL_TIMEOUT_MS) {
        fail(rep, "ETIMEOUT", 408, "upload exceeded the %u s total deadline at %u of %u bytes; the slot was aborted",
             (unsigned)(TOTAL_TIMEOUT_MS / 1000u), (unsigned)written, (unsigned)p.declaredLen);
        failCode = rep.code;
        break;
      }

      // Never ask for more than is outstanding: reading past the declared
      // length is how a body with trailing bytes becomes a corrupt image.
      size_t want = BUF_SIZE;
      if ((uint32_t)want > p.declaredLen - written) {
        want = (size_t)(p.declaredLen - written);
      }
      size_t got = 0;
      ReadStatus rs = read(ctx, buf, want, &got);

      if (rs == READ_TIMEOUT) {
        if ((uint32_t)(millis() - lastDataMs) >= STALL_TIMEOUT_MS) {
          fail(rep, "ETIMEOUT", 408, "no data for %u s at %u of %u bytes; the slot was aborted",
               (unsigned)(STALL_TIMEOUT_MS / 1000u), (unsigned)written, (unsigned)p.declaredLen);
          failCode = rep.code;
          break;
        }
        continue;
      }
      if (rs == READ_ERROR) {
        fail(rep, "ECONN", 400, "the transport failed at %u of %u bytes; the slot was aborted", (unsigned)written,
             (unsigned)p.declaredLen);
        failCode = rep.code;
        break;
      }
      if (rs == READ_EOF || (rs == READ_DATA && got == 0 && want > 0)) {
        // SHORT BODY. The caller declared a length and did not send it, so what
        // is in the slot is a truncated image. Refused here rather than left to
        // esp_ota_end(), so the error names the byte count.
        fail(rep, "ESHORT", 400, "body ended at %u of the declared %u bytes; the slot was aborted", (unsigned)written,
             (unsigned)p.declaredLen);
        failCode = rep.code;
        break;
      }

      lastDataMs = millis();
      mbedtls_sha256_update(&sha, buf, got);
      e = esp_ota_write(handle, buf, got);
      if (e != ESP_OK) {
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
          // Only reachable on the FIRST block: IDF checks the image magic byte
          // there and nowhere else.
          fail(rep, "EMAGIC", 400,
               "the first byte is not an ESP32 image magic (0xE9) — that body is not a firmware.bin");
        } else {
          fail(rep, "EOTAWRITE", 500, "esp_ota_write failed at offset %u: %s; the slot was aborted", (unsigned)written,
               esp_err_to_name(e));
        }
        failCode = rep.code;
        break;
      }
      written += (uint32_t)got;
      st_.written = written;

      // LCD, via activity.h — a direct memory write with no ring behind it, so
      // unlike the bus events this really does animate during the transfer.
      Activity::progressBytes(written, p.declaredLen);
      uint8_t pct = (uint8_t)((uint64_t)written * 100u / p.declaredLen);
      if (nextPoint < PROGRESS_EVENT_POINTS && pct >= PROGRESS_EVENT_PCT[nextPoint]) {
        ProgressEvt ev = {target->label, written, p.declaredLen, pct};
        Bus::emit("ota.upload.progress", fillProgress, &ev);
        nextPoint++;
      }
    }

    rep.written = written;

    // The digest of what was ACTUALLY received, computed either way: a caller
    // that did not declare one still gets it back, which is what makes an
    // upload verifiable after the fact against sha256sum on the host.
    uint8_t digest[32] = {0};
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    toHex(digest, sizeof(digest), rep.sha256);

    if (failCode != nullptr) {
      break;
    }

    // ---- verify BEFORE esp_ota_end() -------------------------------------
    if (p.haveSha) {
      rep.shaChecked = true;
      st_.shaChecked = true;
      rep.shaOk = memcmp(digest, p.sha, sizeof(digest)) == 0;
      st_.shaOk = rep.shaOk;
      if (!rep.shaOk) {
        // The bytes arrived intact as far as TCP is concerned and are still
        // wrong. Abort rather than finalise: esp_ota_end() would very likely
        // ACCEPT them (a corrupt-but-well-formed image passes image validation)
        // and then only the declared digest stands between that and a boot.
        fail(rep, "ESHA256", 400,
             "SHA-256 mismatch: received %s. The slot was aborted and nothing was selected", rep.sha256);
        break;
      }
    }

    // ---- finalise --------------------------------------------------------
    e = esp_ota_end(handle);
    handleOpen = false;  // esp_ota_end frees the handle whatever it returns
    if (e != ESP_OK) {
      if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        fail(rep, "EIMAGE", 400,
             "the uploaded image failed validation (esp_ota_end: %s) — it is not a bootable ESP32-S3 app image. "
             "Nothing was selected",
             esp_err_to_name(e));
      } else {
        fail(rep, "EOTAEND", 500, "esp_ota_end failed: %s; nothing was selected", esp_err_to_name(e));
      }
      break;
    }

    // ---- what did we just install? ---------------------------------------
    //
    // Read back OUT OF THE PARTITION rather than echoed from the request, so
    // the answer to "which build is now in app1" comes from flash.
    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (esp_ota_get_partition_description(target, &desc) == ESP_OK) {
      snprintf(rep.build, sizeof(rep.build), "%s %s", desc.date, desc.time);
      snprintf(rep.idfVersion, sizeof(rep.idfVersion), "%s", desc.idf_ver);
      snprintf(rep.appVersion, sizeof(rep.appVersion), "%s", desc.version);
      snprintf(rep.project, sizeof(rep.project), "%s", desc.project_name);
      snprintf(st_.build, sizeof(st_.build), "%s %s", desc.date, desc.time);
    }

    ok = true;

    // ---- select it, if asked ---------------------------------------------
    //
    // Through OtaHealth::setBootNow(), not a bare esp_ota_set_boot_partition():
    // that function already refuses a slot with no valid image, emits
    // ota.boot_set, and reports which build it chose. Two callers, one
    // implementation, one set of refusals.
    //
    // THIS IS THE STEP THAT ARMS THE SAFETY NET. It writes the slot's otadata
    // entry with ota_state = ESP_OTA_IMG_NEW, so with rollback enabled in the
    // bootloader the next boot of that image is PENDING_VERIFY — which is the
    // state otahealth.cpp's verifyRollbackLater() override exists to preserve,
    // and the state its five criteria then confirm or roll back. Without this
    // the image sits in the slot and nothing ever boots it.
    if (p.select) {
      OtaHealth::BootSetReport bs;
      const char *scode = "EOTA";
      char smsg[128];
      smsg[0] = '\0';
      if (OtaHealth::setBootNow(target->label, bs, &scode, smsg, sizeof(smsg))) {
        rep.selected = true;
        st_.selected = true;
        // rep.rebootRequired is set from otadata at teardown below, which is
        // the authoritative read; bs.rebootRequired says the same thing.
        (void)bs;
      } else {
        // The image is written and valid; only the selection failed. Report the
        // whole truth: ok:false so nobody reboots expecting the new build, with
        // `written` and the build string still in `d`.
        ok = false;
        fail(rep, "ESELECT", 500, "the image was written and validated but could not be selected: %s", smsg);
      }
    }
  } while (false);

  // ---- teardown, on every path -------------------------------------------
  if (handleOpen) {
    // NO HALF-WRITTEN SLOT IS EVER LEFT SELECTED: abort only frees the handle,
    // and nothing above this point has touched otadata. The partial image stays
    // in the inactive slot — harmless, because otadata still names the running
    // one — and the next upload erases it.
    esp_ota_abort(handle);
  }
  free(buf);

  // Whatever otadata says NOW, read back rather than assumed.
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *running = esp_ota_get_running_partition();
  snprintf(rep.bootPartition, sizeof(rep.bootPartition), "%s", boot != nullptr ? boot->label : "?");
  rep.rebootRequired = (boot != nullptr) && (running != nullptr) && strcmp(boot->label, running->label) != 0;

  rep.ok = ok;
  rep.elapsedMs = millis() - startMs;
  if (ok) {
    rep.code = nullptr;
    rep.httpStatus = 200;
    if (rep.msg[0] == '\0') {
      snprintf(rep.msg, sizeof(rep.msg), "wrote %u bytes to '%s'; %s", (unsigned)rep.written, rep.target,
               rep.selected ? "it is now the boot partition — restart to run it (NOT done automatically)"
                            : "otadata was NOT changed, so this image will not boot until it is selected");
    }
  }

  st_.phase = ok ? PH_DONE_OK : PH_DONE_FAIL;
  st_.finishedMs = millis();
  st_.written = rep.written;
  st_.code = rep.code;
  snprintf(st_.msg, sizeof(st_.msg), "%s", rep.msg);

  actEnd(ok);
  Bus::emit("ota.upload.done", fillDone, &rep);
  busy_.store(false, std::memory_order_release);
  return ok;
}

void fillReport(JsonObject d, const Report &rep) {
  // Casts throughout: every one of these is a char[] in the caller's frame,
  // which ArduinoJson 7.4.3 would otherwise store BY POINTER as if it were a
  // literal. See the note above renderActionResult() in console.cpp.
  d["target"] = (const char *)rep.target;
  d["offset"] = rep.targetOffset;
  d["slot_size"] = rep.targetSize;
  d["len"] = rep.declared;
  d["written"] = rep.written;
  d["elapsed_ms"] = rep.elapsedMs;
  d["sha256"] = (const char *)rep.sha256;
  d["sha256_checked"] = rep.shaChecked;
  if (rep.shaChecked) {
    d["sha256_ok"] = rep.shaOk;
  }
  if (rep.build[0] != '\0') {
    // WHICH image is now in that slot, read out of flash after the write.
    d["build"] = (const char *)rep.build;
    d["idf_version"] = (const char *)rep.idfVersion;
    d["app_version"] = (const char *)rep.appVersion;
    d["project"] = (const char *)rep.project;
  }
  // The three facts a caller needs to know what happens next, spelled out
  // rather than implied by the absence of an error.
  d["selected"] = rep.selected;
  d["boot"] = (const char *)rep.bootPartition;
  d["will_boot_uploaded_image"] = rep.selected && strcmp(rep.bootPartition, rep.target) == 0;
  d["reboot_required"] = rep.rebootRequired;
  d["rebooted"] = false;  // never, by design — see the header
  if (rep.msg[0] != '\0') {
    d["msg"] = (const char *)rep.msg;
  }
}

void fillStatus(JsonObject d) {
  const char *phase = "idle";
  switch (st_.phase) {
    case PH_RUNNING:
      phase = "running";
      break;
    case PH_DONE_OK:
      phase = "ok";
      break;
    case PH_DONE_FAIL:
      phase = "failed";
      break;
    default:
      break;
  }
  d["phase"] = phase;
  d["busy"] = busy_.load(std::memory_order_acquire);
  if (st_.phase == PH_IDLE) {
    return;  // nothing has been uploaded this boot; do not invent zeroes
  }
  d["target"] = (const char *)st_.target;
  d["len"] = st_.declared;
  d["written"] = st_.written;
  d["pct"] = st_.declared ? (uint32_t)((uint64_t)st_.written * 100u / st_.declared) : 0u;
  d["select_requested"] = st_.select;
  d["selected"] = st_.selected;
  if (st_.shaChecked) {
    d["sha256_ok"] = st_.shaOk;
  }
  if (st_.phase == PH_RUNNING) {
    d["elapsed_ms"] = millis() - st_.startedMs;
    return;
  }
  d["elapsed_ms"] = st_.finishedMs - st_.startedMs;
  d["finished_ms_ago"] = millis() - st_.finishedMs;
  if (st_.build[0] != '\0') {
    d["build"] = (const char *)st_.build;
  }
  if (st_.code != nullptr) {
    d["code"] = st_.code;
  }
  if (st_.msg[0] != '\0') {
    d["msg"] = (const char *)st_.msg;
  }
}

}  // namespace OtaUpload
