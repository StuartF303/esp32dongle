#include "mod_cdc.h"

#include "bus.h"
#include "console.h"
#include "modauth.h"
#include "protocol.h"

namespace {

bool cdcEnable(const char **errMsg) {
  // Nothing to start. Serial.begin() and Console::begin() ran in setup(),
  // before the registry existed — this module exists to make the console's
  // claim on RES_USB visible to arbitration, not to own its lifecycle.
  (void)errMsg;
  return true;
}

void cdcStatus(JsonObject d) {
  d["transport"] = "cdc";
  d["connected"] = Console::connected();
  d["max_line"] = (uint32_t)Protocol::MAX_LINE;
  d["event_sinks"] = Bus::sinkCount();
#if ARDUINO_USB_MODE == 0
  d["usb_mode"] = "tinyusb";  // composite: CDC survives HID/MSC
#else
  d["usb_mode"] = "usb-serial-jtag";  // fixed function: no HID, no MSC
#endif
}

static_assert(ModAuth::isModuleListed("cdc"), "cdc has no row in ModAuth::MODULES");

const ModuleDescriptor CDC_MODULE = {
    .id = "cdc",
    .name = "USB Serial Console",
    .category = "transport",
    // SHARED, not EXCLUSIVE: see the claim-model note in mod_cdc.h.
    .claims = Claims::claim(Claims::RES_USB, Claims::CLAIM_SHARED),
    .defaultEnabled = true,
    // True and unavoidable: the CDC interface is registered before setup().
    // `essential` is checked first in disable(), so this never routes a
    // disable through the arming path — it just tells a UI the truth.
    .bootTimeBinding = true,
    .essential = true,
    // AUTH_TOKEN to see it in a listing at all, like every other module — even
    // though it has no actions, `status` reports whether the console is
    // connected and which USB mode this build is.
    .minAuth = ModAuth::moduleMinimum("cdc"),
    .enable = cdcEnable,
    .disable = nullptr,  // never called: Registry::disable() refuses first
    .dispatch = nullptr,
    .status = cdcStatus,
    .actions = nullptr,
    .actionCount = 0,
    .tick = nullptr,  // Console::poll is registered by main.cpp: it must run
    .tickIntervalMs = 0,  // even if someone manages to get this module off
};

}  // namespace

const ModuleDescriptor *cdcModuleDescriptor() { return &CDC_MODULE; }
