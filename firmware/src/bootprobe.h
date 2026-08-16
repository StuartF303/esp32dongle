#pragma once
#include <stdint.h>

// Diagnostic: proves ModulePersist::wasEnabledAtBoot() actually works from a
// static constructor on real silicon.
//
// hid's whole arming mechanism rests on that call succeeding from
// do_global_ctors(), where NVS is not yet initialised and the FreeRTOS
// scheduler is not yet running. It fails CLOSED (no HID interface bound), which
// is the safe direction but also the invisible one: if it silently returned
// false forever, arming hid would appear to work and simply never take effect.
//
// Keep this after hid ships. It is a handful of bytes and it is the only thing
// that would tell us the mechanism had broken under a future core update.
namespace BootProbe {

extern volatile bool ctorRan;        // did a static ctor run at all
extern volatile int32_t nvsInitErr;  // esp_err_t from nvs_flash_init() there; 0 == ESP_OK
extern volatile bool ledArmed;       // what wasEnabledAtBoot("led") returned there

}  // namespace BootProbe
