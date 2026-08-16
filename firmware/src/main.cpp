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

#include "console.h"
#include "led.h"
#include "mod_led.h"
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

// Scheduler task: periodic liveness/free-heap event over the JSON console.
// This used to be a plain Serial.printf() line, but once the console is live
// stdout is a JSON-lines protocol — a bare text line here would break any
// host-side line parser (including tools/console.py), so it's an "ev" event
// per ARCHITECTURE.md section 2 instead.
void fillHeartbeatEvent(JsonObject d) {
  d["uptime_ms"] = millis();
  d["free_heap"] = ESP.getFreeHeap();
}

void heartbeatEventTask() {
  Console::sendEvent("heartbeat", fillHeartbeatEvent);
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

  // Register every module, then replay the persisted enable-set. Registration
  // order is also restore order, so the outcome of an unsatisfiable persisted
  // combination is deterministic.
  registry.add(ledModuleDescriptor());
  registry.restoreFromNvs();

  const ModuleRestoreReport &restore = registry.restoreReport();
  if (!restore.nvsRead) {
    // No NVS namespace at all: first boot after a flash or an NVS erase.
    // Bring the default set up so the device isn't silently inert. This is
    // distinguishable from "the user turned everything off", which persists
    // an empty string into an existing namespace.
    ModuleActionResult r;
    registry.enable("led", false, r);
    Serial.printf("\nmodules: no persisted state, defaulting to \"led\" (%s)\n", r.msg);
  } else {
    Serial.printf("\nmodules: restored %u, skipped %u, unknown %u (see the `modules` command)\n",
                  (unsigned)restore.restoredCount, (unsigned)restore.skippedCount, (unsigned)restore.unknownCount);
  }

  scheduler.addTask("led.heartbeat", 500, ledModuleTask);
  scheduler.addTask("heartbeat.event", 5000, heartbeatEventTask);
  scheduler.addTask("console.poll", 0, Console::poll);

  Serial.println();
  Serial.println("setup() complete — entering scheduler loop. Try: help");
}

void loop() {
  scheduler.run();
}
