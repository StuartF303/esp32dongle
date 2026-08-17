// usbdongle W1 — cooperative main loop and command console over USB CDC.
//
// Builds on the W0 bring-up skeleton (chip/partition/OTA banner on boot).
// A non-blocking Scheduler drives everything in loop() instead of delay(),
// Console implements the line-delimited JSON command protocol from
// ../ARCHITECTURE.md section 2 over USB CDC, and the module registry
// (registry.h) owns what is running and which resources it holds. Still no
// LCD, SD, Wi-Fi or BLE.

#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "bus.h"
#include "console.h"
#include "fsmount.h"
#include "led.h"
#include "mod_cdc.h"
#include "mod_display.h"
#include "mod_hid.h"
#include "mod_http.h"
#include "mod_led.h"
#include "mod_storage.h"
#include "otadecide.h"
#include "otahealth.h"
#include "partition_info.h"
#include "registry.h"
#include "scheduler.h"

namespace {

void printPartitionTable() {
  Serial.println();
  Serial.println("--- partition table (as read from flash at runtime) ---");
  Serial.println("type  subtype   offset      size        label");

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t *p = esp_partition_get(it);
    Serial.printf("%-5s %-9s 0x%06lX  0x%06lX    %s\n", p->type == ESP_PARTITION_TYPE_APP ? "app" : "data",
                  partitionSubtypeName(p), (unsigned long)p->address, (unsigned long)p->size, p->label);
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  Serial.println("--------------------------------------------------------");
}

void printRunningPartitionAndOtaState() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  Serial.println();
  Serial.println("--- running partition / OTA state ---");
  if (running == NULL) {
    Serial.println("esp_ota_get_running_partition() returned NULL");
  } else {
    Serial.printf("running: label=\"%s\" offset=0x%06lX size=0x%06lX\n", running->label,
                   (unsigned long)running->address, (unsigned long)running->size);

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK) {
      Serial.printf("ota image state: %s\n", otaStateName(state));
    } else {
      Serial.printf("esp_ota_get_state_partition() failed: %s\n", esp_err_to_name(err));
    }
  }
  Serial.println("--------------------------------------");
}

void printChipInfo() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  Serial.println();
  Serial.println("--- chip ---");
  Serial.printf("model:        %s\n", ESP.getChipModel());
  Serial.printf("revision:     v%d.%d (raw=%d)\n", info.revision / 100, info.revision % 100, info.revision);
  Serial.printf("cores:        %d\n", info.cores);
  Serial.printf("cpu freq:     %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("flash size:   %lu bytes (%.1f MB), as reported by the flash chip itself\n",
                 (unsigned long)ESP.getFlashChipSize(), ESP.getFlashChipSize() / (1024.0 * 1024.0));
  Serial.printf("free heap:    %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("psram:        %s", psramFound() ? "present" : "NOT present (expected on this board)");
  if (psramFound()) {
    Serial.printf(" — %lu bytes", (unsigned long)ESP.getPsramSize());
  }
  Serial.println();
  Serial.println("------------");
}

// Scheduler task: periodic liveness/free-heap event over the event bus.
// This used to be a plain Serial.printf() line, but once the console is live
// stdout is a JSON-lines protocol — a bare text line here would break any
// host-side line parser (including tools/console.py), so it's an "ev" event
// per ARCHITECTURE.md section 2 instead.
//
// Emitted on the BUS, not on the console: when W2 adds WebSocket and BLE
// adapters they subscribe and this line does not change. Nothing outside a
// transport should name a transport.
void fillHeartbeatEvent(JsonObject d, void *ctx) {
  (void)ctx;
  d["uptime_ms"] = millis();
  d["free_heap"] = ESP.getFreeHeap();
}

void heartbeatEventTask() { Bus::emit("heartbeat", fillHeartbeatEvent, nullptr); }

// Registration failures are silent bugs otherwise: a module that never made it
// into the table simply is not there, and the only symptom is a user asking
// where it went.
void addModule(const ModuleDescriptor *desc) {
  if (!registry.add(desc)) {
    Serial.printf("!! registry.add(\"%s\") FAILED — module unavailable this boot\n",
                  (desc && desc->id) ? desc->id : "?");
  }
}

void addTask(const char *name, uint32_t intervalMs, SchedulerTaskFn fn) {
  if (!scheduler.addTask(name, intervalMs, fn)) {
    Serial.printf("!! scheduler.addTask(\"%s\") FAILED — task will never run\n", name);
  }
}

}  // namespace

void setup() {
  Led::begin();

  Serial.begin(115200);
  // Serial is HWCDC here (ARDUINO_USB_MODE=1, native USB-Serial/JTAG). In
  // Arduino-ESP32 3.3.11, HWCDC::write (cores/esp32/HWCDC.cpp:540-623) uses a
  // 256-byte TX ring and, when full, retries xRingbufferSend up to 20 times
  // at tx_timeout_ms (default 100ms) — so one write can block ~2s whenever a
  // host is attached but not draining, which is normal while debugging.
  // Scheduler::run() runs tasks in registration order, so a blocked write in
  // heartbeat.event would delay console.poll and the LED heartbeat behind
  // it. Zero timeout makes the retry loop resolve in microseconds and drops
  // bytes under sustained backpressure instead — the right trade for a
  // debug console.
  Serial.setTxTimeoutMs(0);
  // Native USB-Serial/JTAG CDC: give the host a moment to enumerate before we
  // start printing, so the banner isn't lost off a fresh boot.
  uint32_t waitStart = millis();
  while (!Serial && millis() - waitStart < 3000) {
    delay(10);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" usbdongle W1 — T-Dongle-S3 command console");
  Serial.println("========================================");

  printChipInfo();
  printPartitionTable();
  printRunningPartitionAndOtaState();

  Console::begin();

  // LittleFS, as PLATFORM INFRASTRUCTURE — like NVS, not like a module (backlog
  // F5, fsmount.h). Ordering, both halves load-bearing:
  //   * AFTER Console::begin(), which registers the CDC sink, so the "fs.mount"
  //     event lands in the boot log — the same reason OtaHealth::begin() runs
  //     where it does;
  //   * BEFORE registry.restoreFromNvs(), because `storage` reads the mount
  //     state in its enable() to decide whether it has any volume to serve.
  // It NEVER prevents boot and it NEVER formats: an unformatted partition and a
  // corrupt one are indistinguishable here, so a failure is reported and left
  // alone. `storage format` is the explicit remedy.
  if (!Fs::begin()) {
    Serial.printf("\n!! littlefs: NOT mounted (%s). %s\n", Fs::stageName(), Fs::detail());
  }

  // The lock must exist before the first add(). Everything after this point is
  // guarded; see the threading note in registry.h.
  registry.begin();

  // Register every module, then replay the persisted enable-set. Registration
  // order is also restore order, so the outcome of an unsatisfiable persisted
  // combination is deterministic. Each module's periodic tick is registered
  // with the scheduler by add() from the descriptor — hence `cdc` and `led`
  // going in before the two tasks below, which belong to no module.
  addModule(cdcModuleDescriptor());
  addModule(ledModuleDescriptor());
  // `hid` is bootTimeBinding: its USB HID interface is bound (or not) by its own
  // file-scope static constructor before setup() ran. Registering it here makes
  // its RES_USB claim visible to arbitration and its actions visible to the UI;
  // whether it actually bound this boot is answered by its own enable().
  addModule(hidModuleDescriptor());
  // `storage` mounts the microSD over SDMMC 4-bit and claims RES_SD SHARED.
  // Not bootTimeBinding — SDMMC is a runtime peripheral, so enable()/disable()
  // really do mount and unmount — and not defaultEnabled, so a fresh device
  // does not expose the card's contents until asked.
  addModule(storageModuleDescriptor());
  // `http` is the Wi-Fi SoftAP + HTTP/WebSocket transport. NOT defaultEnabled:
  // it only broadcasts an AP when someone asks it to. It is also the first
  // consumer that runs on a task other than this one — see the threading note
  // at the top of mod_http.cpp and the one in registry.h.
  addModule(httpModuleDescriptor());
  // `display` drives the ST7735 over SPI2_HOST and claims RES_LCD EXCLUSIVE.
  // defaultEnabled — a device that boots to a dark screen looks broken, and
  // the panel is where the pairing PIN has to appear. Registered AFTER `http`
  // so that on a first boot the AP (if it were ever default-enabled) is up
  // before the screen samples it; the display copes either way, since it reads
  // `http`'s status through the registry on every sample rather than once.
  addModule(displayModuleDescriptor());

  // Applies each descriptor's defaultEnabled on a virgin NVS, and SEALS the
  // registry — no module may register after this.
  registry.restoreFromNvs();

  const ModuleRestoreReport &restore = registry.restoreReport();
  if (restore.nvsTooLong) {
    Serial.printf(
        "\n!! modules: persisted set is %u bytes, too long to read — NOTHING was restored.\n"
        "   Only essential modules are up. The next enable/disable rewrites it.\n",
        (unsigned)restore.nvsStoredLen);
  } else if (!restore.nvsRead) {
    // Nothing persisted at all: first boot after a flash or an NVS erase.
    // The descriptors' defaultEnabled decided what came up. Distinguishable
    // from "the user turned everything off", which persists an empty string.
    Serial.printf("\nmodules: no persisted state, applied descriptor defaults (%u up)\n",
                  (unsigned)restore.restoredCount);
  } else {
    Serial.printf("\nmodules: restored %u, skipped %u, unknown %u, armed %u (see the `modules` command)\n",
                  (unsigned)restore.restoredCount, (unsigned)restore.skippedCount, (unsigned)restore.unknownCount,
                  (unsigned)restore.armedCount);
  }

  // OTA rollback confirmation (backlog S4). MUST come after
  // registry.restoreFromNvs() — it asks the registry whether `cdc` came up, and
  // `display` only subscribes to activity.h in its enable(), so beginning
  // earlier would report the pending state to nobody. Also after
  // Console::begin(), which is what puts the CDC sink on the bus for the
  // "ota.pending" event.
  //
  // On a USB flash this does nothing at all: otadata is erased, the running
  // partition reports UNDEFINED, and both begin() and every subsequent tick
  // take an early return.
  OtaHealth::begin();

  addTask("heartbeat.event", 5000, heartbeatEventTask);
  // Not a module tick: the console must keep answering even if the `cdc`
  // module were somehow off, or a mistake would be unrecoverable over USB.
  addTask("console.poll", 0, Console::poll);
  // Also not a module tick, and for a sharper reason: it finishes a deferred
  // HTTP shutdown, which calls httpd_stop() — a JOIN on the server task. A
  // module tick runs inside the registry lock (Registry::tickAt), and joining a
  // task that may be blocked on that lock is a deadlock. This runs outside it.
  // See mod_http.h.
  addTask("http.poll", 20, httpTransportPoll);
  // Not a module tick either, and for the sharpest reason of the three: the
  // OTA health check must run whatever the module set is. Hanging it off a
  // module would mean a disabled module could leave an OTA'd image in
  // PENDING_VERIFY, i.e. silently rolled back at the next restart. Its own
  // run count is also CRIT_TICKS — the evidence that the scheduler is
  // dispatching at all (otadecide.h).
  addTask("ota.health", OtaDecide::DEFAULTS.tickMs, OtaHealth::tick);

  Serial.println();
  Serial.println("setup() complete — entering scheduler loop. Try: help");
}

void loop() {
  scheduler.run();
}
