#include "mod_display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "activity.h"
#include "pairing.h"
#include "screenfmt.h"
#include "scheduler.h"

// ===========================================================================
// Panel: pins, geometry, and where every magic number came from
// ===========================================================================
//
// Pins are CLAUDE.md's verified table, which the schematic confirms:
//   MOSI 3, CLK 5, CS 4, DC 2, RST 1, backlight 38 (ACTIVE LOW), no MISO.
//
// ---- THE OFFSETS AND THE COLOUR SETUP -----------------------------------
//
// A 160x80 ST7735 is a 132x162 controller with a window cut out of it, so it
// needs a column/row offset, and these panels are almost always wired for
// inverted colour and BGR order. Getting any of the three wrong gives a
// shifted, negative or blue-for-red image. None of it is guessed here.
//
// PRIMARY SOURCE — LilyGO's own factory_screen example for this exact board
// (github.com/Xinyuan-LilyGO/T-Dongle-S3, examples/factory_screen/
// factory_screen.ino, the esp_lcd bring-up at lines 206-232):
//
//     esp_lcd_panel_invert_color(panel_handle, true);   // INVON
//     esp_lcd_panel_set_gap(panel_handle, 1, 26);       // x gap 1, y gap 26
//     esp_lcd_panel_swap_xy(panel_handle, true);        // MADCTL MV
//     esp_lcd_panel_mirror(panel_handle, false, true);  // MADCTL MY, not MX
//   with panel_config.color_space = LCD_RGB_ELEMENT_ORDER_BGR and
//   io_config.pclk_hz = 40 MHz, spi_mode 0 (esp_lcd_st7735.h:150-161).
//
// SECONDARY, INDEPENDENT SOURCE — Adafruit_ST7735's INITR_MINI160x80_PLUGIN
// tab (Adafruit_ST7735.cpp): _colstart = 26, _rowstart = 1, an INVON in its
// init list, and the BGR MADCTL branch. Its setRotation(1) computes
// _xstart = _rowstart = 1 and _ystart = _colstart = 26, with
// madctl = MADCTL_MY | MADCTL_MV | MADCTL_BGR.
//
// Those two agree exactly — same 1/26 offsets, same MV|MY, same BGR, same
// inversion — having been derived from different codebases. That is why this
// file uses INITR_MINI160x80_PLUGIN with setRotation(1) rather than an
// open-coded init sequence: the vendor's configuration is already in the
// library, byte for byte.
//
// NOT VERIFIED ON HARDWARE. There is no MISO on this panel (CLAUDE.md), so
// firmware cannot read back a single register to confirm any of it. If the
// image is shifted by a pixel or two, change ROTATION between 1 and 3 (that
// flips which end the FPC ribbon is at); if the colours are negative, the
// panel is a non-plugin variant and INITR_MINI160x80 is the tab to use; if red
// and blue are swapped, it wants the RGB rather than BGR branch, which is the
// same INITR_MINI160x80 change. All three live in the four constants below.

namespace {

constexpr int8_t PIN_MOSI = 3;
constexpr int8_t PIN_SCLK = 5;
constexpr int8_t PIN_CS = 4;
constexpr int8_t PIN_DC = 2;
constexpr int8_t PIN_RST = 1;
constexpr uint8_t PIN_BL = 38;
constexpr int8_t PIN_MISO = -1;  // not wired; the panel is write-only

constexpr uint8_t PANEL_TAB = INITR_MINI160x80_PLUGIN;
constexpr uint8_t ROTATION = 1;  // landscape, FPC to the left. See the note above.
constexpr uint32_t SPI_HZ = 40000000;  // LilyGO's own pclk_hz for this panel

constexpr int16_t W = 160;
constexpr int16_t H = 80;

// Backlight. GPIO38 is ACTIVE LOW (LilyGO: LCD_BK_LIGHT_ON 0 / OFF 1), so the
// LEDC duty is inverted: duty 0 is full brightness, duty 255 is dark. 1 kHz /
// 8 bit are LilyGO's values too (LEDC_BACKLIGHT_FREQ / _BIT_WIDTH).
constexpr uint32_t BL_FREQ_HZ = 1000;
constexpr uint8_t BL_RES_BITS = 8;
constexpr uint8_t BL_DEFAULT_PCT = 100;

// ---- text metrics -------------------------------------------------------
//
// The built-in GFX font is 5x7 in a 6x8 cell at size 1. 160 / 6 = 26 columns,
// exactly; every string that reaches the panel is fitted to a column budget
// derived from these two, never to a literal.
constexpr int16_t CHAR_W = 6;   // advance per character at size 1
constexpr int16_t CHAR_H = 8;   // line height at size 1; the region heights below allow for it
static_assert(W / CHAR_W == 26, "text column budget changed; re-check every fit() call site");

// ---- colours ------------------------------------------------------------
// RGB565. ST77XX_* come from the library; the rest are named here so the
// palette is in one place.
constexpr uint16_t C_BG = ST77XX_BLACK;
constexpr uint16_t C_TEXT = ST77XX_WHITE;
constexpr uint16_t C_DIM = 0x7BEF;   // mid grey
constexpr uint16_t C_BAR = 0x000C;   // dark navy, the header bar
constexpr uint16_t C_OK = ST77XX_GREEN;
constexpr uint16_t C_WARN = ST77XX_YELLOW;
constexpr uint16_t C_ALERT = ST77XX_RED;
constexpr uint16_t C_INFO = ST77XX_CYAN;

// ---- timing -------------------------------------------------------------
//
// TICK_MS is the module's scheduler interval, so it is also the redraw
// ceiling. SAMPLE_MS is how often the world is re-read (the only expensive
// part: one JsonDocument through Registry::statusOf). ANIM_MS is the chevron
// frame rate, and only applies while an activity is running.
constexpr uint32_t TICK_MS = 40;
constexpr uint32_t SAMPLE_MS = 500;
constexpr uint32_t ANIM_MS = 120;
// How long a finished job stays on screen before the footer goes back to
// heap/uptime. A 200 ms verify would otherwise flash past unread.
constexpr uint32_t DONE_LINGER_MS = 2500;
// Regions drawn per tick. The cap is the whole point of the dirty tracker:
// two 160x18 regions is at most ~5,760 bytes of SPI, ~1.2 ms at 40 MHz, which
// leaves the cooperative scheduler its budget. A full repaint therefore takes
// three ticks (120 ms) and is still visually instant.
constexpr uint8_t MAX_DRAW_PER_TICK = 2;

// ===========================================================================
// Screen layout
// ===========================================================================

enum Region : uint8_t {
  REG_ID = 0,  // device name + AP badge
  REG_WIFI,    // AP state and SSID
  REG_NET,     // IP and client count
  REG_AUTH,    // THE PIN, or the session state — see pairing.h for the policy
  REG_MODS,    // enabled modules, with hid called out
  REG_FOOT,    // heap/uptime, or the activity indicator while one is running
  REGION_COUNT
};

struct Rect {
  int16_t x, y, w, h;
};

// constexpr, not const: the static_asserts below read these members at compile
// time, which is only a constant expression for a constexpr object.
constexpr Rect REGION_RECT[REGION_COUNT] = {
    {0, 0, W, 12},   // REG_ID
    {0, 12, W, 11},  // REG_WIFI
    {0, 23, W, 11},  // REG_NET
    {0, 34, W, 18},  // REG_AUTH — 18 px so the PIN can be size 2 (16 px tall)
    {0, 52, W, 11},  // REG_MODS
    {0, 63, W, 17},  // REG_FOOT
};
// Partition arithmetic, done by hand and then checked by the compiler: the
// regions must tile the panel exactly, with no gap (a stripe of stale pixels
// nothing ever redraws) and no overlap (one region silently corrupting its
// neighbour, which the dirty tracker would never notice).
static_assert(REGION_RECT[0].y == 0, "first region must start at the top");
static_assert(REGION_RECT[0].y + REGION_RECT[0].h == REGION_RECT[1].y, "gap/overlap: ID -> WIFI");
static_assert(REGION_RECT[1].y + REGION_RECT[1].h == REGION_RECT[2].y, "gap/overlap: WIFI -> NET");
static_assert(REGION_RECT[2].y + REGION_RECT[2].h == REGION_RECT[3].y, "gap/overlap: NET -> AUTH");
static_assert(REGION_RECT[3].y + REGION_RECT[3].h == REGION_RECT[4].y, "gap/overlap: AUTH -> MODS");
static_assert(REGION_RECT[4].y + REGION_RECT[4].h == REGION_RECT[5].y, "gap/overlap: MODS -> FOOT");
static_assert(REGION_RECT[5].y + REGION_RECT[5].h == H, "regions do not reach the bottom of the panel");
// Every region draws a size-1 line at y+2, so it needs CHAR_H + 2 rows before
// the next region starts. REG_AUTH needs more (size-2 text, 16 px, at y+1) and
// gets 18.
static_assert(REGION_RECT[0].h >= CHAR_H + 2 && REGION_RECT[1].h >= CHAR_H + 2 && REGION_RECT[2].h >= CHAR_H + 2 &&
                  REGION_RECT[4].h >= CHAR_H + 2 && REGION_RECT[5].h >= CHAR_H + 2,
              "a text region is too short for a size-1 line at y+2");
static_assert(REGION_RECT[3].h >= 2 * CHAR_H + 1, "REG_AUTH is too short for the size-2 PIN");
static_assert(REGION_COUNT <= Dirty::MAX_REGIONS, "more regions than the dirty bitmask holds");

enum Screen : uint8_t {
  SCREEN_STATUS = 0,
  SCREEN_DIAG = 1,
  SCREEN_COUNT
};

const char *SCREEN_NAME[SCREEN_COUNT] = {"status", "diag"};

// ===========================================================================
// State
// ===========================================================================

Adafruit_ST7735 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);

bool up_ = false;          // panel initialised and being driven
uint8_t blPct_ = 0;        // backlight 0..100
uint8_t screen_ = SCREEN_STATUS;
char devName_[20] = {0};   // "tdongle-a9d8"

// What the world looked like at the last sample. Rebuilt every SAMPLE_MS;
// the per-region strings are rebuilt from it every tick.
struct World {
  bool httpPresent;
  bool apUp;
  char ssid[24];
  char ip[16];
  uint8_t clients;
  uint8_t sessions;
  char mods[48];
  bool hidOn;
  uint32_t heapFree;
  uint32_t heapMin;
  uint32_t heapMaxBlock;
  uint32_t uptimeMs;
};
World world_ = {};
uint32_t lastSampleMs_ = 0;

// One cached rendered line per region. A region is dirty iff its text OR its
// aux word changed — aux carries the things that are drawn but not printed
// (the HID badge, which auth mode is showing, the chevron frame).
struct RegionState {
  char text[48];
  uint32_t aux;
};
RegionState cache_[REGION_COUNT] = {};
Dirty::Regions dirty_;
bool clearPending_ = false;

uint32_t activitySeenSeq_ = 0;
uint32_t activityDoneMs_ = 0;

// Stats, reported by the `status` action. worstRenderUs_ is the one that
// matters: it is how a display that has quietly started eating the scheduler
// budget gets caught, the same way Scheduler's worstDurationUs does.
uint32_t frames_ = 0;
uint32_t regionWrites_ = 0;
uint32_t fullClears_ = 0;
uint32_t lastRenderUs_ = 0;
uint32_t worstRenderUs_ = 0;

// ===========================================================================
// Backlight
// ===========================================================================

// pct 0 == off. The pin is ACTIVE LOW, so duty is inverted here and nowhere
// else — every other function in this file talks in percent.
void backlightApply(uint8_t pct) {
  if (pct > 100) {
    pct = 100;
  }
  blPct_ = pct;
  uint32_t bright = ((uint32_t)pct * 255u) / 100u;
  ledcWrite(PIN_BL, 255u - bright);
}

void backlightOffHard() {
  // Detach LEDC before forcing the level: leaving the timer attached and
  // writing a duty of 255 would also work, but a plain GPIO high is the state
  // we want the pin left in when this module is not running, so nothing has to
  // reason about who owns the peripheral while the module is disabled.
  ledcDetach(PIN_BL);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);  // ACTIVE LOW: HIGH == backlight off
  blPct_ = 0;
}

// ===========================================================================
// Sampling the world
// ===========================================================================
//
// EVERYTHING HERE GOES THROUGH THE REGISTRY. This module never includes
// mod_http.h and never touches another module's variables: Wi-Fi state arrives
// as the JSON that `http` itself publishes to every transport, so what is on
// the LCD is by construction the same thing the phone and the console see. If
// it is wrong on the panel it is wrong everywhere, which is a far easier bug
// to find than a display with its own private copy of the truth.

void sampleWorld() {
  World w = {};

  {
    // Registry::statusOf takes the same lock list() does, and calls the
    // module's status() only while it is enabled. Called from inside our own
    // tick, i.e. already holding the registry lock — it is recursive, and this
    // is a read-only call, so there is no reentrancy question. Cost is one
    // transient JsonDocument (~1 KB of heap for `http`'s ~30 keys), which is
    // why it happens at SAMPLE_MS and not per tick.
    JsonDocument doc;
    JsonObject o = doc.to<JsonObject>();
    if (registry.statusOf("http", o)) {
      w.httpPresent = true;
      w.apUp = o["ap_up"] | false;
      snprintf(w.ssid, sizeof(w.ssid), "%s", o["ssid"] | "");
      snprintf(w.ip, sizeof(w.ip), "%s", o["ip"] | "");
      w.clients = (uint8_t)(o["clients"] | 0);
      w.sessions = (uint8_t)(o["sessions"] | 0);
    }
  }

  // Enabled modules, from the registry's own accessors. `display` is left out
  // (a lit screen is its own evidence) and `hid` is left out of the text
  // because it gets a badge of its own instead — a keystroke injector being
  // live is not something to render as the fourth word on a grey line.
  size_t used = 0;
  for (uint8_t i = 0; i < registry.count(); i++) {
    if (!registry.enabledAt(i)) {
      continue;
    }
    const ModuleDescriptor *m = registry.at(i);
    if (m == nullptr || m->id == nullptr) {
      continue;
    }
    if (strcmp(m->id, "hid") == 0) {
      w.hidOn = true;
      continue;
    }
    if (strcmp(m->id, "display") == 0) {
      continue;
    }
    int n = snprintf(w.mods + used, sizeof(w.mods) - used, "%s%s", used ? " " : "", m->id);
    if (n < 0 || (size_t)n >= sizeof(w.mods) - used) {
      used = sizeof(w.mods) - 1;  // full; fit() will mark the truncation
      break;
    }
    used += (size_t)n;
  }

  w.heapFree = ESP.getFreeHeap();
  w.heapMin = ESP.getMinFreeHeap();
  w.heapMaxBlock = ESP.getMaxAllocHeap();
  w.uptimeMs = millis();

  world_ = w;
}

// ===========================================================================
// Building the region strings
// ===========================================================================
//
// Pure formatting: given the sampled world plus the live Activity/Pairing
// state, produce the text and the aux word for one region. No drawing here, so
// "did anything change" is answered by comparing strings rather than by
// diffing a dozen fields — which is also why the two screens cost no extra
// dirty-tracking machinery.

// aux encodings, one per region that needs one.
constexpr uint32_t AUX_AUTH_BLANK = 0;
constexpr uint32_t AUX_AUTH_PIN = 1;
constexpr uint32_t AUX_AUTH_PAIRED = 2;

uint8_t animFrame(uint32_t now) { return (uint8_t)((now / ANIM_MS) & 3u); }

// Column budgets, derived rather than written down twice. Every one of these
// is (available pixels) / (character cell), so changing a margin or a font
// size cannot leave a literal behind that lets a string run off the panel.
constexpr size_t COLS_X2 = (size_t)((W - 2) / CHAR_W);          // a line starting at x=2
constexpr size_t COLS_ID = COLS_X2 - 3;                         // less the "AP" badge on the right
constexpr size_t COLS_MODS_HID = COLS_X2 - 4;                   // less the HID badge on the right
constexpr size_t COLS_PIN = (size_t)((W - 26) / (CHAR_W * 2));  // size-2 text starting at x=26
constexpr size_t COLS_ACT = (size_t)((W - 34) / CHAR_W);        // right of the chevrons

// printf into a COLUMN budget rather than a byte budget. Every string on this
// panel goes through here or through ScreenFmt::fit directly: Adafruit_GFX
// does not clip text to a rectangle, so an unbounded snprintf would draw
// straight through the edge of its region.
void boundedf(char *out, size_t cap, size_t cols, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

void boundedf(char *out, size_t cap, size_t cols, const char *fmt, ...) {
  char raw[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(raw, sizeof(raw), fmt, ap);
  va_end(ap);
  ScreenFmt::fit(out, cap, raw, cols);
}

void buildStatus(uint8_t region, uint32_t now, char *out, size_t cap, uint32_t *aux) {
  (void)now;  // only the footer animates
  out[0] = '\0';
  *aux = 0;

  switch (region) {
    case REG_ID:
      ScreenFmt::fit(out, cap, devName_, COLS_ID);
      *aux = world_.apUp ? 1u : 0u;
      break;

    case REG_WIFI:
      *aux = world_.apUp ? 1u : 0u;  // drives the colour, so it must be in the cache key
      if (!world_.httpPresent) {
        ScreenFmt::fit(out, cap, "wifi off", COLS_X2);
      } else if (!world_.apUp) {
        ScreenFmt::fit(out, cap, "wifi: AP down", COLS_X2);
      } else {
        // The SSID is the only string on this screen that is not ours. It is
        // generated (tdongle-xxxx) today, but fitPrefixed keeps "AP " visible
        // and truncates the name rather than the label whatever it becomes.
        ScreenFmt::fitPrefixed(out, cap, "AP ", world_.ssid, COLS_X2);
      }
      break;

    case REG_NET:
      if (world_.apUp && world_.ip[0] != '\0') {
        boundedf(out, cap, COLS_X2, "%s  %u cl", world_.ip, (unsigned)world_.clients);
      } else if (world_.apUp) {
        ScreenFmt::fit(out, cap, "no address", COLS_X2);
      }
      break;

    case REG_AUTH: {
      // THE PIN POLICY LIVES IN pairing.h — read the block comment there
      // before changing this. Short version: it is on screen only while the AP
      // is up and nothing has paired yet, so a device sitting on a desk is not
      // permanently advertising its own pairing secret to passers-by.
      //
      // It is also confined to the status screen. Switching to `diag` hides
      // it, which is a deliberate operator action and reversible in one
      // command; the reverse (a diag screen that leaks the PIN to anyone who
      // sends `screen`) is not.
      char pin[Pairing::MAX_PIN + 1];
      if (Pairing::get(pin, sizeof(pin)) > 0) {
        // COLS_PIN, not COLS_X2: this one is drawn at text size 2, so a
        // character is 12 px wide and only 11 of them fit. AuthFmt::PIN_LEN is
        // 8 today, but the budget is derived rather than assumed.
        ScreenFmt::fit(out, cap, pin, COLS_PIN);
        *aux = AUX_AUTH_PIN;
      } else if (world_.apUp && world_.sessions > 0) {
        boundedf(out, cap, COLS_X2, "paired  %u session%s", (unsigned)world_.sessions,
                 world_.sessions == 1 ? "" : "s");
        *aux = AUX_AUTH_PAIRED;
      } else {
        *aux = AUX_AUTH_BLANK;
      }
      break;
    }

    case REG_MODS: {
      size_t cols = world_.hidOn ? COLS_MODS_HID : COLS_X2;
      ScreenFmt::fitPrefixed(out, cap, "on: ", world_.mods, cols);
      *aux = world_.hidOn ? 1u : 0u;
      break;
    }

    case REG_FOOT:
    default:
      break;
  }
}

void buildDiag(uint8_t region, uint32_t now, char *out, size_t cap, uint32_t *aux) {
  (void)now;
  out[0] = '\0';
  *aux = 0;

  switch (region) {
    case REG_ID:
      ScreenFmt::fit(out, cap, "diagnostics", COLS_ID);
      *aux = world_.apUp ? 1u : 0u;
      break;
    case REG_WIFI:
      boundedf(out, cap, COLS_X2, "heap now %lu", (unsigned long)world_.heapFree);
      break;
    case REG_NET:
      boundedf(out, cap, COLS_X2, "heap min %lu", (unsigned long)world_.heapMin);
      break;
    case REG_AUTH:
      boundedf(out, cap, COLS_X2, "max block %lu", (unsigned long)world_.heapMaxBlock);
      *aux = AUX_AUTH_BLANK;
      break;
    case REG_MODS:
      // Deliberately NOT regionWrites_ or frames_: a counter that this very
      // redraw increments would re-dirty its own region on every tick and the
      // screen would repaint itself forever. worstRenderUs_ converges and then
      // stops moving, which is also the number worth watching.
      boundedf(out, cap, COLS_X2, "tasks %u  worst %luus", (unsigned)scheduler.taskCount(),
               (unsigned long)worstRenderUs_);
      break;
    case REG_FOOT:
    default:
      break;
  }
}

// The footer is shared by both screens: an activity, if one is running, wins
// over the diagnostic line on either. That is the whole reason activity.h
// exists — the thing the device is DOING outranks the thing it is.
void buildFoot(uint32_t now, char *out, size_t cap, uint32_t *aux) {
  Activity::Snapshot a;
  if (Activity::snapshot(a)) {
    const char *tail = (a.state == Activity::DONE_OK) ? "done" : (a.state == Activity::DONE_FAIL) ? "FAILED" : "";
    // COLS_ACT, because this line starts to the right of the chevrons. `who`
    // and `verb` come from another module and can be 12 and 16 characters, so
    // this is the bound that actually gets hit in practice.
    if (tail[0] != '\0') {
      boundedf(out, cap, COLS_ACT, "%s %s %s", a.who, a.verb, tail);
    } else {
      boundedf(out, cap, COLS_ACT, "%s %s %u%%", a.who, a.verb, (unsigned)a.pct);
    }
    // The frame index only enters aux while something is actually running, so
    // a finished or absent job leaves the footer static instead of dirtying it
    // eight times a second forever.
    uint32_t frame = (a.state == Activity::RUNNING) ? animFrame(now) : 0u;
    *aux = (uint32_t)a.state | (frame << 8) | ((uint32_t)a.pct << 16);
    return;
  }

  char heap[12];
  char up[16];
  ScreenFmt::compactBytes(heap, sizeof(heap), world_.heapFree);
  ScreenFmt::uptime(up, sizeof(up), world_.uptimeMs);
  boundedf(out, cap, COLS_X2, "heap %s  up %s", heap, up);
  *aux = 0;
}

void buildRegion(uint8_t region, uint32_t now, char *out, size_t cap, uint32_t *aux) {
  if (region == REG_FOOT) {
    buildFoot(now, out, cap, aux);
    return;
  }
  if (screen_ == SCREEN_DIAG) {
    buildDiag(region, now, out, cap, aux);
  } else {
    buildStatus(region, now, out, cap, aux);
  }
}

// Rebuilds every region's string and marks the ones that moved. Cheap — six
// snprintfs over already-sampled data, a few microseconds — so it runs on
// every tick and the SPI, which is the expensive part, runs only for regions
// that genuinely changed.
void refreshCache(uint32_t now) {
  for (uint8_t r = 0; r < REGION_COUNT; r++) {
    char text[sizeof(cache_[0].text)];
    uint32_t aux = 0;
    buildRegion(r, now, text, sizeof(text), &aux);
    if (aux != cache_[r].aux || strcmp(text, cache_[r].text) != 0) {
      memcpy(cache_[r].text, text, sizeof(text));
      cache_[r].aux = aux;
      Dirty::mark(dirty_, r);
    }
  }
}

// ===========================================================================
// Drawing
// ===========================================================================

void drawText(int16_t x, int16_t y, const char *s, uint16_t colour, uint8_t size) {
  tft.setTextSize(size);
  tft.setTextColor(colour);
  tft.setCursor(x, y);
  tft.print(s);
}

// Four chevrons with one lit, advancing left to right. Stuart asked for
// chevrons specifically; they are also the cheapest possible animation here —
// four fillTriangle calls over a 30x9 patch, i.e. ~540 pixels, ~0.11 ms.
void drawChevrons(int16_t x, int16_t y, uint8_t frame, uint16_t on, uint16_t off) {
  for (uint8_t i = 0; i < 4; i++) {
    int16_t cx = (int16_t)(x + i * 7);
    tft.fillTriangle(cx, y, cx, (int16_t)(y + 8), (int16_t)(cx + 4), (int16_t)(y + 4), (i == frame) ? on : off);
  }
}

void drawRegion(uint8_t r) {
  const Rect &q = REGION_RECT[r];
  const char *text = cache_[r].text;
  const uint32_t aux = cache_[r].aux;

  switch (r) {
    case REG_ID:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BAR);
      drawText(2, (int16_t)(q.y + 2), text, C_TEXT, 1);
      // The AP badge is the one piece of Wi-Fi state visible from across a
      // room: green means the radio is up and this device is reachable.
      drawText((int16_t)(W - 2 - 2 * CHAR_W), (int16_t)(q.y + 2), "AP", aux ? C_OK : C_DIM, 1);
      break;

    case REG_WIFI:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      drawText(2, (int16_t)(q.y + 2), text, aux ? C_TEXT : C_DIM, 1);
      break;

    case REG_NET:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      drawText(2, (int16_t)(q.y + 2), text, C_INFO, 1);
      break;

    case REG_AUTH:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      if (aux == AUX_AUTH_PIN) {
        // Size 2 (12x16 per character): an 8-digit PIN is 96 px, and it is
        // meant to be readable at arm's length from a dongle in the front of a
        // PC. That is the whole justification for an 18 px region.
        drawText(2, (int16_t)(q.y + 5), "PIN", C_DIM, 1);
        drawText(26, (int16_t)(q.y + 1), text, C_WARN, 2);
      } else if (aux == AUX_AUTH_PAIRED) {
        drawText(2, (int16_t)(q.y + 5), text, C_OK, 1);
      }
      break;

    case REG_MODS:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      drawText(2, (int16_t)(q.y + 2), text, C_DIM, 1);
      if (aux) {
        // hid is live: keystrokes can be injected into the host PC right now.
        // ARCHITECTURE.md section 4 makes that reboot-gated and never
        // default-enabled; this is the outward sign that it happened.
        tft.fillRect((int16_t)(W - 22), q.y, 22, q.h, C_ALERT);
        drawText((int16_t)(W - 20), (int16_t)(q.y + 2), "HID", ST77XX_BLACK, 1);
      }
      break;

    case REG_FOOT: {
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      uint8_t state = (uint8_t)(aux & 0xFFu);
      if (state == Activity::IDLE) {
        drawText(2, (int16_t)(q.y + 5), text, C_DIM, 1);
        break;
      }
      uint8_t frame = (uint8_t)((aux >> 8) & 0xFFu);
      uint8_t pct = (uint8_t)((aux >> 16) & 0xFFu);
      uint16_t tint = (state == Activity::DONE_FAIL) ? C_ALERT : (state == Activity::DONE_OK) ? C_OK : C_WARN;
      drawChevrons(2, (int16_t)(q.y + 2), frame, tint, C_BAR);
      drawText(34, (int16_t)(q.y + 3), text, tint, 1);
      // Progress bar: 156 px wide so 1 px is under 1%, and drawn as two
      // fillRects rather than one-per-percent so the SPI cost is fixed.
      int16_t barW = (int16_t)((156 * (int32_t)pct) / 100);
      tft.fillRect(2, (int16_t)(q.y + 13), 156, 3, C_BAR);
      if (barW > 0) {
        tft.fillRect(2, (int16_t)(q.y + 13), barW, 3, tint);
      }
      break;
    }

    default:
      break;
  }
}

// ===========================================================================
// Tick
// ===========================================================================

void displayTick() {
  if (!up_) {
    return;
  }
  uint32_t now = millis();
  uint32_t t0 = micros();

  if (lastSampleMs_ == 0 || (uint32_t)(now - lastSampleMs_) >= SAMPLE_MS) {
    lastSampleMs_ = now;
    sampleWorld();
  }

  // A finished job lingers, then the footer goes back to being a diagnostic
  // line. The producer said "end"; how long that stays legible is the
  // renderer's call, which is why Activity::clear() is on this side.
  uint32_t aseq = Activity::seq();
  if (aseq != activitySeenSeq_) {
    activitySeenSeq_ = aseq;
    Activity::Snapshot a;
    Activity::snapshot(a);
    activityDoneMs_ = (a.state == Activity::DONE_OK || a.state == Activity::DONE_FAIL) ? now : 0;
  }
  if (activityDoneMs_ != 0 && (uint32_t)(now - activityDoneMs_) >= DONE_LINGER_MS) {
    activityDoneMs_ = 0;
    Activity::clear();
    activitySeenSeq_ = Activity::seq();
  }

  refreshCache(now);

  if (clearPending_) {
    // Full repaint: one 25,600-byte transfer, ~5.1 ms at 40 MHz. It happens on
    // enable, on `refresh` and on a screen change — never on a state change —
    // so it is a bounded one-off rather than something in the steady-state
    // path. All six regions then repaint over the next three ticks
    // (REGION_COUNT / MAX_DRAW_PER_TICK), i.e. 120 ms.
    tft.fillScreen(C_BG);
    clearPending_ = false;
    fullClears_++;
    Dirty::markAll(dirty_);
  }

  uint8_t drawn = 0;
  while (drawn < MAX_DRAW_PER_TICK) {
    uint8_t r = Dirty::takeNext(dirty_);
    if (r >= REGION_COUNT) {
      break;
    }
    drawRegion(r);
    drawn++;
    regionWrites_++;
  }

  frames_++;
  lastRenderUs_ = micros() - t0;
  if (lastRenderUs_ > worstRenderUs_) {
    worstRenderUs_ = lastRenderUs_;
  }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void buildDeviceName() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  // Same shape as the AP's SSID (mod_http builds "tdongle-%02x%02x" from the
  // same two bytes) so the name on the panel and the name in the Wi-Fi list
  // are recognisably the same device. Derived here from the MAC rather than
  // read from `http`, because it is a hardware fact, not another module's
  // state — and it has to be right even with the radio off.
  snprintf(devName_, sizeof(devName_), "tdongle-%02x%02x", mac[4], mac[5]);
}

bool displayEnable(const char **errMsg) {
  if (up_) {
    return true;
  }

  buildDeviceName();

  // Explicit pins BEFORE the library touches SPI. Adafruit_SPITFT::initSPI()
  // calls _spi->begin() with no arguments, and Arduino-ESP32's SPIClass::begin
  // returns true immediately once the bus is up (SPI.cpp:66-69) — so THIS call
  // is what decides the pin mapping, and it must come first. Without it the bus
  // would come up on the ESP32-S3 defaults, whose SCK is GPIO12: the SD card's
  // clock line on this board.
  //
  // The global `SPI` is SPIClass(FSPI), and FSPI is 0 on the S3, i.e. SPI2_HOST
  // (esp32-hal-spi.h:35) — the same host CLAUDE.md and LilyGO's example name.
  if (!SPI.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, -1)) {
    // Genuinely unable to start the bus: say so rather than reporting a
    // successful enable of a module that is driving nothing.
    if (errMsg != nullptr) {
      *errMsg = "SPI2_HOST would not start (GPIO 5/3); the panel cannot be driven";
    }
    return false;
  }

  tft.initR(PANEL_TAB);
  tft.setRotation(ROTATION);
  tft.setSPISpeed(SPI_HZ);
  tft.setTextWrap(false);  // regions clip by construction; wrapping would spill into the next one

  // NOTE: this cannot fail loudly the way registry.h asks a bring-up to. There
  // is no MISO on this panel (CLAUDE.md), so there is no register to read back
  // and no way for firmware to distinguish "initialised" from "wrote 30 bytes
  // into a disconnected ribbon". The honest failure indicator is the backlight
  // coming on with nothing on the glass.
  up_ = true;

  // Start subscribed: from here on, other modules' Activity calls and http's
  // Pairing publications stop being no-ops. Both are cleared first so nothing
  // stale from a previous enable appears.
  Activity::subscribe(false);
  Activity::subscribe(true);
  Pairing::subscribe(false);
  Pairing::subscribe(true);
  activitySeenSeq_ = Activity::seq();
  activityDoneMs_ = 0;

  memset(cache_, 0, sizeof(cache_));
  Dirty::init(dirty_, REGION_COUNT);
  Dirty::markAll(dirty_);
  lastSampleMs_ = 0;
  clearPending_ = false;
  screen_ = SCREEN_STATUS;

  // BLANK BEFORE LIGHTING. The ST7735's own frame RAM holds whatever was in it
  // at power-on, and the first tick is up to TICK_MS away — so turning the
  // backlight on first shows a frame of noise on every enable. One 5.1 ms
  // fillScreen, once, buys a clean start; the regions paint in over the next
  // three ticks (MAX_DRAW_PER_TICK).
  tft.fillScreen(C_BG);
  fullClears_++;

  if (ledcAttach(PIN_BL, BL_FREQ_HZ, BL_RES_BITS)) {
    backlightApply(BL_DEFAULT_PCT);
  } else {
    // No LEDC channel free (there are 8). Fall back to a plain GPIO: full
    // brightness only, no `backlight pct`, but a lit screen — which beats
    // failing the whole enable over a dimmer.
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, LOW);  // ACTIVE LOW: LOW == backlight on
    blPct_ = 100;
  }
  return true;
}

bool displayDisable(const char **errMsg) {
  (void)errMsg;
  if (!up_) {
    backlightOffHard();
    return true;
  }

  // Order matters: stop the producers first (so nothing publishes a PIN into a
  // buffer nobody is going to clear), blank the glass, kill the backlight,
  // then give the bus back.
  Activity::subscribe(false);
  Pairing::subscribe(false);  // also wipes the PIN out of pairing.h's buffer

  tft.fillScreen(C_BG);
  backlightOffHard();

  // Release SPI2_HOST properly rather than just leaving the panel dark. The
  // claim on RES_LCD is given up the moment this returns, and a module that
  // still owned the bus after releasing its claim is exactly the kind of leak
  // the claim model exists to make impossible.
  SPI.end();

  up_ = false;
  memset(cache_, 0, sizeof(cache_));
  Dirty::clearAll(dirty_);
  return true;
}

// ===========================================================================
// Actions
// ===========================================================================
//
// Note what is NOT here: `text`, `draw`, `image`, `pixel`, `bitmap`. See the
// block comment in mod_display.h — the screen is device-owned, and that is a
// property of the auth design, not an unfinished feature.
//
// THREADING: a dispatch arriving over WebSocket runs on the HTTP server's task
// (priority 5), not the loop task — so `screen` and `refresh` mutate cache_,
// dirty_ and clearPending_ from a different core to the one displayTick()
// reads them on. That is safe for exactly one reason: Registry::dispatch() and
// Registry::tickAt() both hold the registry lock across the callback
// (registry.h), so the two can never run at the same time. Nothing here does
// its own locking, and nothing here should start without revisiting that.
// No SPI is touched from a dispatch either — actions set flags, the tick draws.

DispatchResult displayDispatch(const CmdContext &ctx, const char *act, JsonObjectConst p, JsonObject d, CmdError *err) {
  if (strcmp(act, "status") == 0) {
    d["panel"] = up_;
    d["screen"] = SCREEN_NAME[screen_ < SCREEN_COUNT ? screen_ : 0];
    d["width"] = W;
    d["height"] = H;
    d["backlight_on"] = blPct_ > 0;
    d["backlight_pct"] = blPct_;
    d["spi_hz"] = SPI_HZ;
    d["tick_ms"] = TICK_MS;
    d["regions"] = REGION_COUNT;
    d["dirty"] = Dirty::pending(dirty_);
    d["frames"] = frames_;
    d["region_writes"] = regionWrites_;
    d["full_clears"] = fullClears_;
    d["last_render_us"] = lastRenderUs_;
    d["worst_render_us"] = worstRenderUs_;
    // Whether the PIN is on screen, NEVER the PIN itself: this response goes
    // out over the same WebSocket the PIN exists to avoid.
    d["pin_on_screen"] = Pairing::visible();
    Activity::Snapshot a;
    if (Activity::snapshot(a)) {
      JsonObject act_ = d["activity"].to<JsonObject>();
      // NOT cast to const char*: `a` is a stack local, and ArduinoJson stores
      // a const char* by POINTER without copying. char[] decays to char*,
      // which it does copy.
      act_["who"] = a.who;
      act_["verb"] = a.verb;
      act_["pct"] = a.pct;
      act_["state"] = a.state == Activity::RUNNING    ? "running"
                      : a.state == Activity::DONE_OK  ? "done"
                                                      : "failed";
    }
    return DISPATCH_OK;
  }

  if (strcmp(act, "backlight") == 0) {
    // AUTH_TOKEN, because turning the backlight off makes a working device
    // look dead — and because a device whose screen an unauthenticated client
    // can blank cannot be trusted as the out-of-band channel that shows the
    // PIN. Stuart's call; `screen` and `refresh` are deliberately NOT gated,
    // since neither can hide or fake anything the operator needs.
    if (ctx.authLevel < AUTH_TOKEN) {
      cmdErrorf(err, "EAUTH", "backlight needs auth >= token; '%s' is at level %u", ctx.transport ? ctx.transport : "?",
                (unsigned)ctx.authLevel);
      return DISPATCH_FAIL;
    }
    if (!up_) {
      cmdErrorf(err, "ENOTUP", "the panel is not initialised");
      return DISPATCH_FAIL;
    }

    JsonVariantConst pct = p["pct"];
    JsonVariantConst on = p["on"];
    if (!pct.isNull()) {
      if (!pct.is<uint32_t>() || pct.as<uint32_t>() > 100) {
        cmdErrorf(err, "EARGS", "p.pct must be an integer 0..100");
        return DISPATCH_FAIL;
      }
      backlightApply((uint8_t)pct.as<uint32_t>());
    } else if (!on.isNull()) {
      if (!on.is<bool>()) {
        cmdErrorf(err, "EARGS", "p.on must be true or false");
        return DISPATCH_FAIL;
      }
      backlightApply(on.as<bool>() ? BL_DEFAULT_PCT : 0);
    } else {
      cmdErrorf(err, "EARGS", "expected p:{on:true|false} or p:{pct:0..100}");
      return DISPATCH_FAIL;
    }
    d["backlight_on"] = blPct_ > 0;
    d["backlight_pct"] = blPct_;
    return DISPATCH_OK;
  }

  if (strcmp(act, "screen") == 0) {
    const char *name = p["name"] | (const char *)nullptr;
    if (name == nullptr) {
      cmdErrorf(err, "EARGS", "missing p.name (\"status\" or \"diag\")");
      return DISPATCH_FAIL;
    }
    for (uint8_t i = 0; i < SCREEN_COUNT; i++) {
      if (strcmp(name, SCREEN_NAME[i]) == 0) {
        if (screen_ != i) {
          screen_ = i;
          clearPending_ = true;
          memset(cache_, 0, sizeof(cache_));
        }
        d["screen"] = SCREEN_NAME[i];
        return DISPATCH_OK;
      }
    }
    // Naming the valid set matters here: the whole point of firmware-defined
    // screens is that the caller cannot invent one, so the error has to say
    // what does exist.
    cmdErrorf(err, "EARGS", "unknown screen \"%.16s\"; firmware defines \"status\" and \"diag\"", name);
    return DISPATCH_FAIL;
  }

  if (strcmp(act, "refresh") == 0) {
    if (!up_) {
      cmdErrorf(err, "ENOTUP", "the panel is not initialised");
      return DISPATCH_FAIL;
    }
    clearPending_ = true;
    memset(cache_, 0, sizeof(cache_));
    d["queued"] = true;
    return DISPATCH_OK;
  }

  cmdErrorf(err, "EUNKNOWN", "unknown action for module 'display': \"%.16s\" (try \"status\")", act);
  return DISPATCH_FAIL;
}

void displayStatus(JsonObject d) {
  d["panel"] = up_;
  d["screen"] = SCREEN_NAME[screen_ < SCREEN_COUNT ? screen_ : 0];
  d["backlight_pct"] = blPct_;
  d["frames"] = frames_;
  d["region_writes"] = regionWrites_;
  d["worst_render_us"] = worstRenderUs_;
  d["pin_on_screen"] = Pairing::visible();
}

const ModuleAction DISPLAY_ACTIONS[] = {
    {"status", "panel, backlight, current screen and redraw statistics", ""},
    {"backlight", "backlight on/off or 0..100% (auth >= token)", "on:true|false | pct:0..100"},
    {"screen", "select a firmware-defined screen", "name:\"status\"|\"diag\""},
    {"refresh", "force a full repaint", ""},
};

const ModuleDescriptor DISPLAY_MODULE = {
    .id = "display",
    .name = "Status LCD",
    .category = "status",
    .claims = Claims::claim(Claims::RES_LCD, Claims::CLAIM_EXCLUSIVE),
    // ON out of the box. A device that boots to a blank screen looks broken,
    // and the LCD is also where the pairing PIN has to appear for the auth
    // flow in ARCHITECTURE.md section 4 to work at all.
    .defaultEnabled = true,
    // SPI and the panel are ordinary runtime peripherals — nothing here is
    // frozen at boot the way a TinyUSB interface is.
    .bootTimeBinding = false,
    .essential = false,
    .enable = displayEnable,
    .disable = displayDisable,
    .dispatch = displayDispatch,
    .status = displayStatus,
    .actions = DISPLAY_ACTIONS,
    .actionCount = (uint8_t)(sizeof(DISPLAY_ACTIONS) / sizeof(DISPLAY_ACTIONS[0])),
    .tick = displayTick,
    .tickIntervalMs = TICK_MS,
};

}  // namespace

const ModuleDescriptor *displayModuleDescriptor() { return &DISPLAY_MODULE; }
