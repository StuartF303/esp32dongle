#include "fsmount.h"

#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"

namespace {

Fs::Stage stage_ = Fs::FS_NOT_TRIED;
int32_t lastErr_ = 0;
bool mounted_ = false;
uint32_t offset_ = 0;
uint32_t size_ = 0;
// Long form. Reported through storage.caps/status; sized like mod_storage.cpp's
// mountDetail for the same reason — the sentence has to say what to DO.
char detail_[288] = {0};

const esp_partition_t *findPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, Fs::LABEL);
}

// Runs the register call and records exactly why it failed. Shared by begin()
// and the remount half of format(), so the two cannot describe the same
// failure differently.
bool mountNow() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = Fs::MOUNT;
  conf.partition_label = Fs::LABEL;
  conf.partition = nullptr;
  // FALSE, and it must stay false. See the block at the top of fsmount.h: an
  // unformatted partition and a corrupt one look identical here, and only one
  // of those two is safe to answer by erasing it.
  conf.format_if_mount_failed = 0;
  conf.read_only = 0;
  conf.dont_mount = 0;
  // FALSE as well: grow_on_mount rewrites the filesystem's own idea of its size
  // to match the partition. That is a WRITE to a volume we have just been told
  // we do not understand, and this partition's size is fixed by partitions.csv
  // anyway.
  conf.grow_on_mount = 0;

  esp_err_t e = esp_vfs_littlefs_register(&conf);
  lastErr_ = (int32_t)e;
  if (e == ESP_OK) {
    mounted_ = true;
    stage_ = Fs::FS_OK;
    return true;
  }
  mounted_ = false;
  switch (e) {
    case ESP_ERR_NOT_FOUND:
      stage_ = Fs::FS_NO_PARTITION;
      break;
    case ESP_ERR_NO_MEM:
      stage_ = Fs::FS_NO_MEM;
      break;
    case ESP_FAIL:
      stage_ = Fs::FS_MOUNT_FAILED;
      break;
    default:
      stage_ = Fs::FS_UNKNOWN;
      break;
  }
  return false;
}

void describe() {
  switch (stage_) {
    case Fs::FS_OK: {
      size_t total = 0, used = 0;
      if (esp_littlefs_info(Fs::LABEL, &total, &used) == ESP_OK) {
        snprintf(detail_, sizeof(detail_), "mounted at %s from partition '%s' (0x%06lX, %lu KB); %lu of %lu bytes used",
                 Fs::MOUNT, Fs::LABEL, (unsigned long)offset_, (unsigned long)(size_ / 1024u), (unsigned long)used,
                 (unsigned long)total);
      } else {
        snprintf(detail_, sizeof(detail_), "mounted at %s from partition '%s' (0x%06lX, %lu KB)", Fs::MOUNT, Fs::LABEL,
                 (unsigned long)offset_, (unsigned long)(size_ / 1024u));
      }
      break;
    }
    case Fs::FS_NO_PARTITION:
      snprintf(detail_, sizeof(detail_),
               "no partition labelled '%s' in the live table. This image was flashed against a different "
               "partitions.csv, or the table at 0x8000 is not ours — check `parts`.",
               Fs::LABEL);
      break;
    case Fs::FS_NO_MEM:
      snprintf(detail_, sizeof(detail_),
               "LittleFS could not allocate its caches (esp_err 0x%x). Free heap was too low at boot; nothing was "
               "written and a reboot may well succeed.",
               (unsigned)lastErr_);
      break;
    case Fs::FS_MOUNT_FAILED:
      // The one that matters, and the one that must NOT be answered by
      // formatting on the spot.
      snprintf(detail_, sizeof(detail_),
               "partition '%s' is present (0x%06lX, %lu KB) but would not mount. It is either NEVER FORMATTED (normal "
               "on a first flash) or CORRUPT — those are indistinguishable here, so nothing was erased. Run `storage "
               "format p:{volume:\"fs\",confirm:true}` over USB to make it an empty filesystem.",
               Fs::LABEL, (unsigned long)offset_, (unsigned long)(size_ / 1024u));
      break;
    case Fs::FS_NOT_TRIED:
      snprintf(detail_, sizeof(detail_), "Fs::begin() has not run yet");
      break;
    default:
      snprintf(detail_, sizeof(detail_),
               "esp_vfs_littlefs_register() returned an unexpected esp_err 0x%x; the failing stage is not being "
               "guessed at",
               (unsigned)lastErr_);
      break;
  }
}

struct MountEvent {
  bool mounted;
  const char *stage;
  int32_t err;
  uint32_t total;
  uint32_t used;
};

void fillMountEvent(JsonObject d, void *ctx) {
  const MountEvent *e = (const MountEvent *)ctx;
  if (e == nullptr) {
    return;
  }
  d["volume"] = "fs";
  d["mountpoint"] = Fs::MOUNT;
  d["mounted"] = e->mounted;
  d["stage"] = e->stage;
  if (e->err != 0) {
    d["err"] = e->err;
  }
  if (e->mounted) {
    d["total"] = e->total;
    d["used"] = e->used;
  }
}

}  // namespace

namespace Fs {

bool begin() {
  if (mounted_) {
    return true;  // idempotent
  }
  stage_ = FS_NOT_TRIED;
  lastErr_ = 0;
  offset_ = 0;
  size_ = 0;
  detail_[0] = '\0';

  // Look the partition up FIRST, so "there is no such partition" is reported as
  // itself rather than as a mount failure. It also gives the offset/size the
  // failure messages quote, which is how someone tells a stale partition table
  // from a bad filesystem.
  const esp_partition_t *p = findPartition();
  if (p == nullptr) {
    stage_ = FS_NO_PARTITION;
    describe();
    return false;
  }
  offset_ = p->address;
  size_ = p->size;

  mountNow();
  describe();

  size_t total = 0, used = 0;
  if (mounted_) {
    esp_littlefs_info(LABEL, &total, &used);
  }
  MountEvent e = {mounted_, stageName(), lastErr_, (uint32_t)total, (uint32_t)used};
  Bus::emit("fs.mount", fillMountEvent, &e);
  return mounted_;
}

bool mounted() { return mounted_; }
Stage stage() { return stage_; }
int32_t lastErr() { return lastErr_; }
const char *detail() { return detail_; }
uint32_t partitionOffset() { return offset_; }
uint32_t partitionSize() { return size_; }

const char *stageName() {
  switch (stage_) {
    case FS_OK:
      return "ok";
    case FS_NOT_TRIED:
      return "not_tried";
    case FS_NO_PARTITION:
      return "no_partition";
    case FS_NO_MEM:
      return "no_mem";
    case FS_MOUNT_FAILED:
      return "mount_failed";
    default:
      return "unknown";
  }
}

bool info(uint64_t *total, uint64_t *used) {
  if (!mounted_) {
    return false;
  }
  size_t t = 0, u = 0;
  if (esp_littlefs_info(LABEL, &t, &u) != ESP_OK) {
    return false;
  }
  if (total != nullptr) {
    *total = t;
  }
  if (used != nullptr) {
    *used = u;
  }
  return true;
}

bool format(const char **code, char *msg, size_t cap) {
  if (findPartition() == nullptr) {
    if (code != nullptr) {
      *code = "ENOPART";
    }
    if (msg != nullptr && cap > 0) {
      snprintf(msg, cap, "there is no partition labelled '%s' in the live table; nothing was touched", LABEL);
    }
    return false;
  }

  // Unmount FIRST rather than relying on esp_littlefs_format() to notice. It
  // does handle a mounted partition itself, but doing it here means the state
  // this file reports is the state this file put things in, on both the success
  // and the failure path.
  if (mounted_) {
    esp_err_t u = esp_vfs_littlefs_unregister(LABEL);
    if (u != ESP_OK && u != ESP_ERR_INVALID_STATE) {
      lastErr_ = (int32_t)u;
      if (code != nullptr) {
        *code = "EUNMOUNT";
      }
      if (msg != nullptr && cap > 0) {
        snprintf(msg, cap, "could not unmount %s before formatting: %s; nothing was erased", MOUNT,
                 esp_err_to_name(u));
      }
      return false;
    }
    mounted_ = false;
  }

  esp_err_t e = esp_littlefs_format(LABEL);
  if (e != ESP_OK) {
    lastErr_ = (int32_t)e;
    stage_ = FS_MOUNT_FAILED;
    // Try to get back to wherever we were, so a failed format does not also
    // cost the mount that was working before it.
    mountNow();
    describe();
    if (code != nullptr) {
      *code = "EFORMAT";
    }
    if (msg != nullptr && cap > 0) {
      snprintf(msg, cap, "esp_littlefs_format('%s') failed: %s; the volume is %s", LABEL, esp_err_to_name(e),
               mounted_ ? "mounted again" : "NOT mounted");
    }
    return false;
  }

  bool ok = mountNow();
  describe();
  if (!ok) {
    if (code != nullptr) {
      *code = "EREMOUNT";
    }
    if (msg != nullptr && cap > 0) {
      snprintf(msg, cap, "the partition formatted but would not mount afterwards (esp_err 0x%x); reboot to retry",
               (unsigned)lastErr_);
    }
    return false;
  }

  size_t total = 0, used = 0;
  esp_littlefs_info(LABEL, &total, &used);
  MountEvent ev = {true, stageName(), 0, (uint32_t)total, (uint32_t)used};
  Bus::emit("fs.format", fillMountEvent, &ev);
  if (msg != nullptr && cap > 0) {
    snprintf(msg, cap, "formatted and remounted at %s; %lu bytes free of %lu", MOUNT,
             (unsigned long)(total - used), (unsigned long)total);
  }
  return true;
}

void fillStatus(JsonObject d) {
  d["mounted"] = mounted_;
  d["mountpoint"] = MOUNT;
  d["label"] = LABEL;
  d["stage"] = stageName();
  if (lastErr_ != 0) {
    d["stage_err"] = lastErr_;
  }
  d["offset"] = offset_;
  d["partition_size"] = size_;
  d["detail"] = (const char *)detail_;
}

}  // namespace Fs
