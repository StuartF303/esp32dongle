#include "mod_hid.h"

#include <Arduino.h>
#include <Preferences.h>
#include <USBHIDKeyboard.h>
#include <new>  // placement new
#include <stdint.h>
#include <string.h>
#include <strings.h>  // strcasecmp

#include "bus.h"
#include "keyboard_layout_en_gb.h"

namespace {

// ===========================================================================
// FILE-SCOPE BINDING — the whole reason this module exists the way it does.
// ===========================================================================
//
// A plain global `USBHIDKeyboard kbd;` would run its constructor in
// do_global_ctors() unconditionally. That constructor calls
// tinyusb_enable_interface2() (USBHID::USBHID), which registers the HID
// interface into the composite descriptor set — so the device would enumerate
// as a keyboard on EVERY boot, armed or not. That defeats the entire
// reboot-gated safety model (a keystroke injector present whenever the dongle is
// plugged in) and spends a USB endpoint/interface budget we may not want.
//
// So the object is constructed CONDITIONALLY, from a file-scope static
// constructor, only when the user has explicitly armed the module and rebooted —
// exactly the mechanism proven on real silicon in bootprobe.cpp
// (ModulePersist::wasEnabledAtBoot runs, nvs_flash_init returns ESP_OK
// pre-scheduler, and the value tracks live NVS across reboots).
//
// Placement-new into a static byte buffer, NOT heap `new`: this runs before
// app_main and before the scheduler, at the most delicate point of boot. A
// static buffer needs no allocator, cannot fail, and cannot depend on heap-init
// ordering. The interface is bound iff `kbd != nullptr` for the whole life of
// this image; nothing at runtime can bind or unbind it.
alignas(USBHIDKeyboard) uint8_t kbdStorage[sizeof(USBHIDKeyboard)];
USBHIDKeyboard *kbd = nullptr;

struct HidBinder {
  HidBinder() {
    if (ModulePersist::wasEnabledAtBoot("hid")) {
      kbd = new (kbdStorage) USBHIDKeyboard();
    }
  }
};
HidBinder binder;  // constructed in do_global_ctors(), before USB.begin()

// ===========================================================================
// Layout
// ===========================================================================

enum LayoutId : uint8_t { LAYOUT_EN_GB = 0, LAYOUT_EN_US = 1 };

// Active layout for THIS host. Set in hidEnable() from NVS; changeable at
// runtime via the `layout` action. Default en_GB.
LayoutId currentLayout = LAYOUT_EN_GB;

const uint8_t *layoutPtr(LayoutId id) {
  return id == LAYOUT_EN_US ? KeyboardLayout_en_US : KeyboardLayout_en_GB;
}
const char *layoutName(LayoutId id) { return id == LAYOUT_EN_US ? "en_US" : "en_GB"; }

const char *NVS_NS = "hid";
const char *NVS_LAYOUT_KEY = "layout";

LayoutId readLayoutSetting() {
  LayoutId r = LAYOUT_EN_GB;  // default: never fail open to the wrong host layout
  Preferences prefs;
  if (prefs.begin(NVS_NS, true)) {
    char b[8] = {0};
    size_t n = prefs.getString(NVS_LAYOUT_KEY, b, sizeof(b));
    prefs.end();
    if (n > 0 && strcmp(b, "en_US") == 0) {
      r = LAYOUT_EN_US;
    }
  }
  return r;
}

void writeLayoutSetting(LayoutId id) {
  Preferences prefs;
  if (prefs.begin(NVS_NS, false)) {
    prefs.putString(NVS_LAYOUT_KEY, layoutName(id));
    prefs.end();
  }
}

// ===========================================================================
// Raw HID usages for the NONUS_HASH workaround (see keyboard_layout_en_gb.cpp)
// ===========================================================================

// HID_KEY_SHIFT_LEFT — pressRaw() treats 0xE0..0xE7 as modifier bits.
const uint8_t RAW_LEFT_SHIFT = 0xE1;
// Keyboard Non-US # and ~. Cannot be produced through the asciimap on en_GB
// (its table byte 0x32 is the ISO_REPLACEMENT sentinel for usage 0x64), so the
// pump presses the raw usage instead.
const uint8_t USAGE_NONUS_HASH = 0x32;

// ===========================================================================
// Typing pump state — a single bounded job, driven from the registry tick.
// ===========================================================================
//
// `type` returns DISPATCH_ACCEPTED and drips one character per tick: ~8 ms per
// two HID reports means a long macro would hold the cooperative scheduler for
// tens of seconds if run inline. The tick is registry-registered and gated on
// the enabled flag, so it stops dead when the module is disabled.
//
// tick and dispatch are serialised by the same recursive registry mutex (both
// enter through the registry lock), so this state needs no extra guard.

const size_t MAX_TYPE_LEN = 512;   // bounded queue; over-length is rejected, never truncated
const uint32_t DEFAULT_DELAY_MS = 0;  // as fast as the tick allows (~one char per tickIntervalMs)

uint8_t typeBuf[MAX_TYPE_LEN];
size_t jobLen = 0;
size_t jobIndex = 0;
uint32_t perCharDelayMs = DEFAULT_DELAY_MS;
uint32_t nextEmitMs = 0;
bool jobActive = false;
uint32_t jobCounter = 0;    // last issued job id
uint32_t currentJobId = 0;
uint32_t currentReqId = 0;  // request id, for event correlation
uint32_t jobCharsSent = 0;  // characters emitted in the current job
uint32_t totalCharsSent = 0;  // lifetime, across all jobs since boot

// Emit ONE character as a complete press+release, honouring the active layout.
void emitChar(char c) {
  if (kbd == nullptr) {
    return;
  }
  if (c == '\r') {
    return;  // CR is a no-op; '\n' below maps to Enter via the asciimap
  }
  if (currentLayout == LAYOUT_EN_GB && (c == '#' || c == '~')) {
    // NONUS_HASH cannot go through kbd->write(): see keyboard_layout_en_gb.cpp.
    if (c == '~') {
      kbd->pressRaw(RAW_LEFT_SHIFT);
    }
    kbd->pressRaw(USAGE_NONUS_HASH);
    kbd->releaseAll();
    return;
  }
  kbd->write((uint8_t)c);  // stock path: uses kbd's asciimap set in hidEnable()
}

void fillDone(JsonObject d, void *ctx) {
  (void)ctx;
  d["mod"] = "hid";
  d["job"] = currentJobId;
  if (currentReqId != 0) {
    d["id"] = currentReqId;  // correlate with the accepted request
  }
  d["chars"] = (uint32_t)jobLen;
}

void hidTick() {
  if (!jobActive) {
    return;
  }
  uint32_t now = millis();
  if ((int32_t)(now - nextEmitMs) < 0) {
    return;  // pacing: not due yet
  }
  emitChar((char)typeBuf[jobIndex]);
  jobIndex++;
  jobCharsSent++;
  totalCharsSent++;
  nextEmitMs = now + perCharDelayMs;
  if (jobIndex >= jobLen) {
    jobActive = false;
    Bus::emit("hid.done", fillDone, nullptr);
  }
}

// ===========================================================================
// Named keys for the `key` action
// ===========================================================================

struct NamedKey {
  const char *name;
  uint8_t code;  // press()-style code (KEY_* from USBHIDKeyboard.h)
};

const NamedKey NAMED_KEYS[] = {
    {"enter", KEY_RETURN},      {"return", KEY_RETURN},   {"esc", KEY_ESC},
    {"escape", KEY_ESC},        {"tab", KEY_TAB},         {"space", KEY_SPACE},
    {"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE}, {"del", KEY_DELETE},
    {"insert", KEY_INSERT},     {"home", KEY_HOME},       {"end", KEY_END},
    {"pageup", KEY_PAGE_UP},    {"pagedown", KEY_PAGE_DOWN}, {"up", KEY_UP_ARROW},
    {"down", KEY_DOWN_ARROW},   {"left", KEY_LEFT_ARROW}, {"right", KEY_RIGHT_ARROW},
    {"capslock", KEY_CAPS_LOCK}, {"numlock", KEY_NUM_LOCK}, {"scrolllock", KEY_SCROLL_LOCK},
    {"printscreen", KEY_PRINT_SCREEN}, {"pause", KEY_PAUSE}, {"menu", KEY_MENU},
    {"f1", KEY_F1},   {"f2", KEY_F2},   {"f3", KEY_F3},   {"f4", KEY_F4},
    {"f5", KEY_F5},   {"f6", KEY_F6},   {"f7", KEY_F7},   {"f8", KEY_F8},
    {"f9", KEY_F9},   {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
    {"f13", KEY_F13}, {"f14", KEY_F14}, {"f15", KEY_F15}, {"f16", KEY_F16},
    {"f17", KEY_F17}, {"f18", KEY_F18}, {"f19", KEY_F19}, {"f20", KEY_F20},
    {"f21", KEY_F21}, {"f22", KEY_F22}, {"f23", KEY_F23}, {"f24", KEY_F24},
};

int lookupNamedKey(const char *name) {
  for (const NamedKey &k : NAMED_KEYS) {
    if (strcasecmp(name, k.name) == 0) {
      return (int)k.code;
    }
  }
  return -1;
}

// Modifier bitfield used only for parsing/reporting p.mods.
enum { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_GUI = 8 };

// ===========================================================================
// Auth
// ===========================================================================

// Injection is refused below AUTH_TOKEN. Policy lives here, in the module, not
// in the transport: a fresh Wi-Fi/BLE client (AUTH_NONE) must not be able to
// type into the host, and only the module knows that about itself.
bool requireInjectAuth(const CmdContext &ctx, CmdError *err) {
  if (ctx.authLevel < AUTH_TOKEN) {
    cmdErrorf(err, "EAUTH",
              "hid injection requires an authenticated session (auth >= token); transport '%s' is at level %u and is "
              "refused.",
              ctx.transport ? ctx.transport : "?", (unsigned)ctx.authLevel);
    return false;
  }
  return true;
}

// ===========================================================================
// Status
// ===========================================================================

void fillStatus(JsonObject d) {
  // `armed` is the persisted intent for the NEXT boot; `bound` is whether the
  // interface actually got constructed THIS boot. pending == armed && !bound.
  bool armed = ModulePersist::wasEnabledAtBoot("hid");
  bool bound = (kbd != nullptr);
  d["armed"] = armed;
  d["bound"] = bound;
  d["pending_restart"] = armed != bound;
  d["layout"] = layoutName(currentLayout);
  d["host_dependent"] = true;  // layout is a property of the host, not the dongle
  d["job_active"] = jobActive;
  d["job"] = jobActive ? currentJobId : 0;
  d["queue_depth"] = jobActive ? (uint32_t)(jobLen - jobIndex) : 0;
  d["job_chars_sent"] = jobCharsSent;
  d["chars_sent_total"] = totalCharsSent;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool hidEnable(const char **errMsg) {
  // FAIL LOUDLY, never silently. If the interface was not bound this boot the
  // host sees no keyboard; returning true here would show "on" in the UI with
  // nothing behind it — the exact quiet failure the registry contract (see the
  // TinyUSB block in registry.h) warns about.
  if (kbd == nullptr) {
    *errMsg =
        "hid is armed but its USB HID interface was NOT bound this boot (the file-scope keyboard object was not "
        "constructed). This binds only at boot; reboot the device to apply.";
    return false;
  }

  currentLayout = readLayoutSetting();
  kbd->begin(layoutPtr(currentLayout));  // sets the asciimap and inits the HID device (idempotent)
  kbd->releaseAll();                       // start from a clean report

  // Fresh enable starts with no job queued.
  jobActive = false;
  jobIndex = 0;
  jobLen = 0;
  return true;
}

bool hidDisable(const char **errMsg) {
  (void)errMsg;
  // We stop SENDING, and drop any queued job. We CANNOT withdraw the USB HID
  // interface: it was frozen into the composite descriptor set by USB.begin()
  // before setup(), and TinyUSB on this framework has no runtime interface
  // teardown. The host still sees a keyboard until the next reboot — this call
  // just makes it go quiet and clears any stuck key/modifier.
  //
  // In practice the registry routes runtime disable() of a bootTimeBinding
  // module through its disarm path and never calls this; it is here for
  // completeness and to state the limitation plainly.
  jobActive = false;
  if (kbd != nullptr) {
    kbd->releaseAll();
  }
  return true;
}

// ===========================================================================
// Dispatch
// ===========================================================================

DispatchResult actType(const CmdContext &ctx, JsonObjectConst p, JsonObject d, CmdError *err) {
  if (!requireInjectAuth(ctx, err)) {
    return DISPATCH_FAIL;
  }
  if (kbd == nullptr) {  // defensive: enable() would have refused, but never inject blind
    cmdErrorf(err, "EREBOOT", "hid interface is not bound this boot; reboot to use it");
    return DISPATCH_FAIL;
  }
  const char *text = p["text"] | (const char *)nullptr;
  if (text == nullptr) {
    cmdErrorf(err, "EARGS", "missing p.text (the string to type)");
    return DISPATCH_FAIL;
  }
  size_t len = strlen(text);
  if (len == 0) {
    cmdErrorf(err, "EARGS", "p.text is empty");
    return DISPATCH_FAIL;
  }
  if (len > MAX_TYPE_LEN) {
    cmdErrorf(err, "EARGS", "p.text is %u chars; the limit is %u. Rejected rather than truncated — split the macro.",
              (unsigned)len, (unsigned)MAX_TYPE_LEN);
    return DISPATCH_FAIL;
  }
  if (jobActive) {
    cmdErrorf(err, "EBUSY", "a type job is already running (job %u, %u of %u chars sent); wait for hid.done or call release",
              currentJobId, jobCharsSent, (unsigned)jobLen);
    return DISPATCH_FAIL;
  }
  // Reject non-ASCII / stray control bytes rather than inject undefined
  // keystrokes: the asciimap is 7-bit (KeyboardLayout.h: "Non-ASCII characters
  // are not supported"), and a byte >0x7e would be misread by press() as some
  // arbitrary key. Tab and newline are allowed; everything else printable.
  for (size_t i = 0; i < len; i++) {
    uint8_t b = (uint8_t)text[i];
    if (b == '\t' || b == '\n' || b == '\r') {
      continue;
    }
    if (b < 0x20 || b > 0x7e) {
      cmdErrorf(err, "EARGS",
                "p.text has a non-typeable byte 0x%02x at offset %u (this layout is 7-bit ASCII; tab/newline ok)",
                (unsigned)b, (unsigned)i);
      return DISPATCH_FAIL;
    }
  }

  // Pacing. delay_ms wins if both are present; otherwise wpm (5 chars/word) or
  // the default. Bounded so a caller cannot request a pathological rate.
  uint32_t delay = DEFAULT_DELAY_MS;
  if (!p["delay_ms"].isNull()) {
    long v = p["delay_ms"].as<long>();
    if (v < 0) v = 0;
    if (v > 60000) v = 60000;
    delay = (uint32_t)v;
  } else if (!p["wpm"].isNull()) {
    long wpm = p["wpm"].as<long>();
    if (wpm < 1) wpm = 1;
    if (wpm > 2000) wpm = 2000;
    delay = (uint32_t)(12000L / wpm);  // 60000ms / (wpm * 5 chars)
  }

  memcpy(typeBuf, text, len);
  jobLen = len;
  jobIndex = 0;
  jobCharsSent = 0;
  perCharDelayMs = delay;
  currentJobId = ++jobCounter;
  currentReqId = ctx.reqId;
  nextEmitMs = millis();
  jobActive = true;

  d["job"] = currentJobId;
  d["chars"] = (uint32_t)len;
  d["delay_ms"] = perCharDelayMs;
  return DISPATCH_ACCEPTED;  // completion arrives as the hid.done event
}

DispatchResult actKey(const CmdContext &ctx, JsonObjectConst p, JsonObject d, CmdError *err) {
  if (!requireInjectAuth(ctx, err)) {
    return DISPATCH_FAIL;
  }
  if (kbd == nullptr) {
    cmdErrorf(err, "EREBOOT", "hid interface is not bound this boot; reboot to use it");
    return DISPATCH_FAIL;
  }
  const char *key = p["key"] | (const char *)nullptr;
  if (key == nullptr || key[0] == '\0') {
    cmdErrorf(err, "EARGS", "missing p.key (e.g. \"Enter\", \"F5\", \"a\")");
    return DISPATCH_FAIL;
  }

  // Parse modifiers first, so an invalid one changes nothing on the host.
  uint8_t mods = 0;
  JsonArrayConst ma = p["mods"];
  if (!ma.isNull()) {
    for (JsonVariantConst m : ma) {
      const char *s = m.as<const char *>();
      if (s == nullptr) continue;
      if (strcasecmp(s, "ctrl") == 0) mods |= MOD_CTRL;
      else if (strcasecmp(s, "shift") == 0) mods |= MOD_SHIFT;
      else if (strcasecmp(s, "alt") == 0) mods |= MOD_ALT;
      else if (strcasecmp(s, "gui") == 0 || strcasecmp(s, "win") == 0 || strcasecmp(s, "cmd") == 0) mods |= MOD_GUI;
      else {
        cmdErrorf(err, "EARGS", "unknown modifier \"%.16s\" (use ctrl/alt/shift/gui)", s);
        return DISPATCH_FAIL;
      }
    }
  }

  // Resolve the main key BEFORE pressing anything.
  bool isChar = (strlen(key) == 1);
  if (isChar) {
    uint8_t b = (uint8_t)key[0];
    if (b < 0x20 || b > 0x7e) {
      cmdErrorf(err, "EARGS", "p.key is a non-typeable byte 0x%02x (use a printable ASCII char or a named key)",
                (unsigned)b);
      return DISPATCH_FAIL;
    }
  }
  int namedCode = -1;
  if (!isChar) {
    namedCode = lookupNamedKey(key);
    if (namedCode < 0) {
      cmdErrorf(err, "EARGS", "unknown key \"%.24s\" (a single character, or a named key like Enter/Esc/Tab/F5/Up)",
                key);
      return DISPATCH_FAIL;
    }
  }

  // Press modifiers, then the key, then release everything.
  if (mods & MOD_CTRL) kbd->press(KEY_LEFT_CTRL);
  if (mods & MOD_SHIFT) kbd->press(KEY_LEFT_SHIFT);
  if (mods & MOD_ALT) kbd->press(KEY_LEFT_ALT);
  if (mods & MOD_GUI) kbd->press(KEY_LEFT_GUI);

  if (isChar) {
    char c = key[0];
    if (currentLayout == LAYOUT_EN_GB && (c == '#' || c == '~')) {
      if (c == '~') kbd->pressRaw(RAW_LEFT_SHIFT);
      kbd->pressRaw(USAGE_NONUS_HASH);  // NONUS_HASH: see keyboard_layout_en_gb.cpp
    } else {
      kbd->press((uint8_t)c);
    }
  } else {
    kbd->press((uint8_t)namedCode);
  }
  kbd->releaseAll();

  d["key"] = key;
  JsonArray outMods = d["mods"].to<JsonArray>();
  if (mods & MOD_CTRL) outMods.add("ctrl");
  if (mods & MOD_SHIFT) outMods.add("shift");
  if (mods & MOD_ALT) outMods.add("alt");
  if (mods & MOD_GUI) outMods.add("gui");
  return DISPATCH_OK;
}

DispatchResult actRelease(const CmdContext &ctx, JsonObject d, CmdError *err) {
  (void)ctx;  // panic stop: allowed at ANY auth level — it can only make the keyboard idle
  (void)err;
  bool cancelled = jobActive;
  jobActive = false;
  if (kbd != nullptr) {
    kbd->releaseAll();
  }
  d["released"] = true;
  d["cancelled_job"] = cancelled;
  return DISPATCH_OK;
}

DispatchResult actLayout(const CmdContext &ctx, JsonObjectConst p, JsonObject d, CmdError *err) {
  const char *set = p["set"] | (const char *)nullptr;
  if (set != nullptr) {
    // Changing which host layout we assume is a configuration change; gate it
    // like injection so an unauthenticated client cannot silently corrupt every
    // future macro's output.
    if (!requireInjectAuth(ctx, err)) {
      return DISPATCH_FAIL;
    }
    LayoutId want;
    if (strcasecmp(set, "en_GB") == 0) want = LAYOUT_EN_GB;
    else if (strcasecmp(set, "en_US") == 0) want = LAYOUT_EN_US;
    else {
      cmdErrorf(err, "EARGS", "unknown layout \"%.16s\" (en_GB or en_US)", set);
      return DISPATCH_FAIL;
    }
    currentLayout = want;
    writeLayoutSetting(want);
    if (kbd != nullptr) {
      kbd->begin(layoutPtr(want));  // refresh the asciimap; USBHID::begin is idempotent
    }
  }
  d["layout"] = layoutName(currentLayout);
  d["host_dependent"] = true;
  d["available"] = "en_GB,en_US";
  return DISPATCH_OK;
}

DispatchResult hidDispatch(const CmdContext &ctx, const char *act, JsonObjectConst p, JsonObject d, CmdError *err) {
  if (strcmp(act, "type") == 0) return actType(ctx, p, d, err);
  if (strcmp(act, "key") == 0) return actKey(ctx, p, d, err);
  if (strcmp(act, "release") == 0) return actRelease(ctx, d, err);
  if (strcmp(act, "layout") == 0) return actLayout(ctx, p, d, err);
  if (strcmp(act, "status") == 0) {
    fillStatus(d);
    return DISPATCH_OK;
  }
  cmdErrorf(err, "EUNKNOWN", "unknown action for module 'hid': \"%.16s\" (type/key/release/status/layout)", act);
  return DISPATCH_FAIL;
}

void hidStatus(JsonObject d) { fillStatus(d); }

// Static .rodata; this is what lets the web UI render the module's controls
// without any module-specific front-end code (ARCHITECTURE.md §2). Every
// entry below is read off actType/actKey/actLayout, NOT off the prose hints
// they replace — see modparam.h.
//
// `wpm` and `delay_ms` are BOTH optional and neither is a "oneOf": actType
// accepts either, and delay_ms wins when both are present. That is what the
// help strings say, because inventing a mutual-exclusion concept the dispatch
// does not enforce would be a second description of the code rather than the
// code's own.
const ModuleParam TYPE_PARAMS[] = {
    ModParam::str("text", true,
                  "the string to type: 1..512 characters, printable ASCII plus tab and newline. Longer is "
                  "rejected, never truncated; CR is ignored."),
    ModParam::num("wpm", false, "typing speed in words per minute (5 chars/word). Clamped to this range. Ignored if delay_ms is given.",
                  1, 2000),
    ModParam::num("delay_ms", false, "per-character delay in ms. Clamped to this range. Takes precedence over wpm; default 0.", 0,
                  60000),
};

const ModuleParam KEY_PARAMS[] = {
    ModParam::str("key", true,
                  "one printable ASCII character, or a named key: Enter/Return, Esc, Tab, Space, Backspace, "
                  "Delete, Insert, Home, End, PageUp, PageDown, Up, Down, Left, Right, CapsLock, NumLock, "
                  "ScrollLock, PrintScreen, Pause, Menu, F1..F24. Case-insensitive."),
    // A JSON ARRAY, which is why P_ENUM_LIST exists (modparam.h). Sent as a
    // string it would parse to a null JsonArrayConst and the modifiers would
    // vanish with no error at all.
    ModParam::choiceList("mods", false, "modifiers held while the key is pressed. \"win\" and \"cmd\" are accepted as aliases of gui.",
                         "ctrl,shift,alt,gui"),
};

const ModuleParam LAYOUT_PARAMS[] = {
    ModParam::choice("set", false,
                     "omit to READ the current layout. Setting it needs auth >= token and persists to NVS.",
                     "en_GB,en_US"),
};

const ModuleAction HID_ACTIONS[] = {
    {"type", "type a string into the host (queued; completes with a hid.done event)", MOD_PARAMS(TYPE_PARAMS)},
    {"key", "send one keystroke, optionally with modifiers", MOD_PARAMS(KEY_PARAMS)},
    {"release", "releaseAll() — panic stop for a stuck key or modifier", nullptr, 0},
    {"status", "armed/bound/layout/queue depth/current job/chars sent", nullptr, 0},
    {"layout", "get or set the HOST keyboard layout (en_GB default, en_US selectable)", MOD_PARAMS(LAYOUT_PARAMS)},
};

const ModuleDescriptor HID_MODULE = {
    .id = "hid",
    .name = "USB Keyboard",
    .category = "input",
    // USB is SHARED (composite CDC+HID+MSC on this framework); it never blocks
    // another module. hid/msc mutual exclusion, if wanted, is policy, not a
    // claim — see mod_cdc.h and ARCHITECTURE.md §2.
    .claims = Claims::claim(Claims::RES_USB, Claims::CLAIM_SHARED),
    // MUST be false: never default-arm a keystroke injector.
    .defaultEnabled = false,
    // Its USB interface binds from a static constructor before setup(); runtime
    // enable/disable records intent and reports pendingRestart.
    .bootTimeBinding = true,
    .essential = false,
    .enable = hidEnable,
    .disable = hidDisable,
    .dispatch = hidDispatch,
    .status = hidStatus,
    .actions = HID_ACTIONS,
    .actionCount = (uint8_t)(sizeof(HID_ACTIONS) / sizeof(HID_ACTIONS[0])),
    // The typing pump. Registered with the scheduler and gated on enabled by the
    // registry; the module neither registers nor gates it. ~8 ms per char is the
    // report round-trip, so one char per tick keeps each scheduler pass bounded.
    .tick = hidTick,
    .tickIntervalMs = 8,
};

}  // namespace

const ModuleDescriptor *hidModuleDescriptor() { return &HID_MODULE; }
