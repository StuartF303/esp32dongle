#include "bootprobe.h"

#include <nvs_flash.h>

#include "registry.h"

namespace BootProbe {
volatile bool ctorRan = false;
volatile int32_t nvsInitErr = -1;
volatile bool ledArmed = false;
}  // namespace BootProbe

namespace {

// File-scope object. Its constructor runs in do_global_ctors() — the SAME place
// a module's USBHID/USBMSC object is constructed, and the only hook that exists
// before USB.begin() freezes the descriptor set in app_main().
//
// At this point:
//   - the FreeRTOS scheduler has not started
//   - initArduino() has not run, so nvs_flash_init() has NOT been called
//
// wasEnabledAtBoot() calls nvs_flash_init() itself for exactly that reason. We
// record its error code separately here so a failure is distinguishable from
// "the module simply was not armed" — both return false, and conflating them is
// how this would silently rot.
struct BootProbeCtor {
  BootProbeCtor() {
    BootProbe::ctorRan = true;
    // Idempotent; initArduino()'s later call then returns ESP_OK immediately.
    BootProbe::nvsInitErr = (int32_t)nvs_flash_init();
    BootProbe::ledArmed = ModulePersist::wasEnabledAtBoot("led");
  }
};

BootProbeCtor probeInstance;

}  // namespace
