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
#include "modauth.h"
#include "pairing.h"
#include "qrfit.h"
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
// The QR's own two colours, and they are not part of the palette above on
// purpose: a QR decoder wants DARK MODULES ON A LIGHT FIELD, including the
// quiet zone. Inverting it (white modules on the black UI background) decodes
// on some readers and not others, and "works on my phone" is not a property
// worth having in the one path that exists to make pairing reliable. So the
// block is a white square on an otherwise dark screen, which is also the
// highest-contrast thing this panel can produce.
constexpr uint16_t C_QR_DARK = ST77XX_BLACK;
constexpr uint16_t C_QR_LIGHT = ST77XX_WHITE;

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
// Rows of the QR block pushed per SPI transaction. 8 rows of 80 px is 1,280
// bytes of stack in drawQr(). MEASURED 2026-08-24, THIS IS SLOWER THAN 1, not
// faster — the comment in drawQr() carries both arms' numbers and why the
// original reasoning was wrong. Left at 8 pending stuart's call rather than
// changed quietly.
constexpr int16_t QR_ROWS_PER_BAND = 8;

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
  REG_QR,      // the pairing QR block. EMPTY on every screen but SCREEN_PAIR.
  REGION_COUNT
};

struct Rect {
  int16_t x, y, w, h;
};

// A region that does not exist on this screen. Not drawn, not built, not
// dirtied — see rectFor() and the empty-rect guards in buildRegion/drawRegion.
// This is what lets one region ID mean "the QR block" on one screen and
// "nothing at all" on another, without a second dirty tracker.
constexpr Rect NO_RECT = {0, 0, 0, 0};

constexpr bool rectEmpty(const Rect &a) { return a.w <= 0 || a.h <= 0; }

constexpr bool rectInPanel(const Rect &a) {
  return a.x >= 0 && a.y >= 0 && a.x + a.w <= W && a.y + a.h <= H;
}

constexpr bool rectsOverlap(const Rect &a, const Rect &b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// ---- the tiling proof, generalised -------------------------------------
//
// The old proof was six hand-written "region N ends where region N+1 begins"
// asserts, and it worked because BOTH screens were the same stack of
// full-width horizontal stripes. The pairing screen is not: it is a square QR
// block beside a narrow column, so "ends where the next one begins" is not
// even the right question about it.
//
// Rather than weaken the guarantee to fit the new layout, this states the
// property the stripe asserts were really proving — every pixel of the panel
// belongs to exactly one region — in a form that holds for any arrangement:
//
//     no rect leaves the panel  +  no two rects overlap  +  areas sum to W*H
//         =>  exact tiling, no gap and no overlap
//
// A gap would be a stripe of stale pixels that nothing ever redraws; an
// overlap would be one region silently corrupting its neighbour, which the
// dirty tracker would never notice because it believes the neighbour is clean.
// Both are compile-time errors here. Empty rects are skipped: they contribute
// no area and cover no pixel, so a screen that omits a region still has to
// account for every pixel with the ones it keeps.
constexpr bool tilesPanelExactly(const Rect (&r)[REGION_COUNT]) {
  int32_t area = 0;
  for (size_t i = 0; i < REGION_COUNT; i++) {
    if (rectEmpty(r[i])) {
      continue;
    }
    if (!rectInPanel(r[i])) {
      return false;
    }
    area += (int32_t)r[i].w * (int32_t)r[i].h;
    for (size_t j = i + 1; j < REGION_COUNT; j++) {
      if (!rectEmpty(r[j]) && rectsOverlap(r[i], r[j])) {
        return false;
      }
    }
  }
  return area == (int32_t)W * (int32_t)H;
}

// ---- screen 1 and 2: the stripe layout, unchanged ----------------------
//
// constexpr, not const: the static_asserts below read these members at compile
// time, which is only a constant expression for a constexpr object.
constexpr Rect STATUS_RECT[REGION_COUNT] = {
    {0, 0, W, 12},   // REG_ID
    {0, 12, W, 11},  // REG_WIFI
    {0, 23, W, 11},  // REG_NET
    {0, 34, W, 18},  // REG_AUTH — 18 px so the PIN can be size 2 (16 px tall)
    {0, 52, W, 11},  // REG_MODS
    {0, 63, W, 17},  // REG_FOOT
    NO_RECT,         // REG_QR — status and diag never show one
};
// The original per-boundary asserts are kept alongside the general one above.
// They are redundant as proofs and are not redundant as ERROR MESSAGES: a
// failing tilesPanelExactly() says only "these no longer tile", where these
// name the boundary that moved.
static_assert(STATUS_RECT[0].y == 0, "first region must start at the top");
static_assert(STATUS_RECT[0].y + STATUS_RECT[0].h == STATUS_RECT[1].y, "gap/overlap: ID -> WIFI");
static_assert(STATUS_RECT[1].y + STATUS_RECT[1].h == STATUS_RECT[2].y, "gap/overlap: WIFI -> NET");
static_assert(STATUS_RECT[2].y + STATUS_RECT[2].h == STATUS_RECT[3].y, "gap/overlap: NET -> AUTH");
static_assert(STATUS_RECT[3].y + STATUS_RECT[3].h == STATUS_RECT[4].y, "gap/overlap: AUTH -> MODS");
static_assert(STATUS_RECT[4].y + STATUS_RECT[4].h == STATUS_RECT[5].y, "gap/overlap: MODS -> FOOT");
static_assert(STATUS_RECT[5].y + STATUS_RECT[5].h == H, "regions do not reach the bottom of the panel");
// Every region draws a size-1 line at y+2, so it needs CHAR_H + 2 rows before
// the next region starts. REG_AUTH needs more (size-2 text, 16 px, at y+1) and
// gets 18.
static_assert(STATUS_RECT[0].h >= CHAR_H + 2 && STATUS_RECT[1].h >= CHAR_H + 2 && STATUS_RECT[2].h >= CHAR_H + 2 &&
                  STATUS_RECT[4].h >= CHAR_H + 2 && STATUS_RECT[5].h >= CHAR_H + 2,
              "a text region is too short for a size-1 line at y+2");
static_assert(STATUS_RECT[3].h >= 2 * CHAR_H + 1, "REG_AUTH is too short for the size-2 PIN");
static_assert(tilesPanelExactly(STATUS_RECT), "the status/diag regions no longer tile the panel exactly");

// ---- screen 3: pairing -------------------------------------------------
//
// 160 px of width splits into the QR block on the left and a narrow column on
// the right. The block is SQUARE AND PANEL-HEIGHT because that is what bounds
// the QR version (qrfit.h): a square symbol on a 160x80 panel is constrained
// by the short side, so 80 px of height is 80 px of block, and the width the
// code cannot use is width the digits can.
constexpr int16_t QR_BLOCK = (int16_t)QrFit::BLOCK_PX;
static_assert(QrFit::BLOCK_PX == H, "qrfit sizes the code to the panel HEIGHT; the two have diverged");

constexpr int16_t PAIR_COL_X = QR_BLOCK;
constexpr int16_t PAIR_COL_W = W - QR_BLOCK;  // 80

// Region order here is by ID, not by position — REG_AUTH sits between REG_WIFI
// and REG_NET on the glass. The IDs keep their MEANING across screens (AUTH is
// always the PIN, NET is always the address or the equivalent hint), which is
// what lets drawRegion keep one switch instead of gaining a second one.
constexpr Rect PAIR_RECT[REGION_COUNT] = {
    {PAIR_COL_X, 0, PAIR_COL_W, 12},   // REG_ID   — what to DO, as a title bar
    {PAIR_COL_X, 12, PAIR_COL_W, 12},  // REG_WIFI — the SSID (join) or the IP (pair)
    {PAIR_COL_X, 68, PAIR_COL_W, 12},  // REG_NET  — one-line hint, at the bottom
    {PAIR_COL_X, 24, PAIR_COL_W, 44},  // REG_AUTH — "PIN" over four LARGE digits
    NO_RECT,                           // REG_MODS — no room, and not what this screen is for
    NO_RECT,                           // REG_FOOT — likewise; see the note below
    {0, 0, QR_BLOCK, H},               // REG_QR
};
// WHAT THE PAIR SCREEN GIVES UP, said plainly: the module list and the
// activity/heap footer. 80 px of column cannot carry them next to digits this
// size, and this screen only exists while the device is unpaired and idle —
// there is no activity to indicate, because nothing has authenticated yet to
// start one. It is also transient: the moment a session exists, Pairing
// withdraws and the status screen (which has both) comes back.
static_assert(tilesPanelExactly(PAIR_RECT), "the pairing regions no longer tile the panel exactly");
static_assert(PAIR_RECT[REG_QR].w == QR_BLOCK && PAIR_RECT[REG_QR].h == H,
              "the QR block must be the full panel height; qrfit's scale arithmetic assumes it");
static_assert(REGION_COUNT <= Dirty::MAX_REGIONS, "more regions than the dirty bitmask holds");

// SPENDING THE LEGIBILITY GAIN. The whole justification for dropping the PIN
// from 8 digits to 4 (ARCHITECTURE.md, "Pairing model") was that four digits
// can be drawn about twice as tall — and until now nothing had collected it:
// the status screen still draws the PIN at size 2 in an 18 px stripe, so the
// change had cost 10^8 -> 10^4 of search space and bought nothing.
//
// Size 3 is 18x24 per character. Four digits is 72 px, which fits the 80 px
// column; size 4 would be 24x32, i.e. 96 px, which does not. The second
// assert is the one that matters — it fails if a future layout change ever
// makes a LARGER size fit, so this stays the biggest the panel will take
// rather than quietly staying at 3 forever.
constexpr uint8_t PAIR_PIN_SIZE = 3;
static_assert(4 * CHAR_W * PAIR_PIN_SIZE <= PAIR_COL_W, "the PIN at this text size no longer fits beside the QR");
static_assert(4 * CHAR_W * (PAIR_PIN_SIZE + 1) > PAIR_COL_W, "a larger PIN text size would now fit; use it");
static_assert(PAIR_RECT[REG_AUTH].h >= CHAR_H + 2 + PAIR_PIN_SIZE * CHAR_H,
              "the pair screen's PIN region is too short for a size-1 label over size-3 digits");

enum Screen : uint8_t {
  SCREEN_STATUS = 0,
  SCREEN_DIAG = 1,
  // The whole vocabulary of the `screen` action. Everything at or above this
  // is firmware-selected and cannot be named over the wire.
  SCREEN_NAMED_COUNT,
  // THE PAIRING SCREEN IS DELIBERATELY UNNAMEABLE. It appears on its own while
  // Pairing::visible() and vanishes when pairing withdraws; there is no
  // `screen pair`, and there must not be. See the comment in displayTick where
  // it is selected, and the REG_AUTH note about `diag`.
  SCREEN_PAIR = SCREEN_NAMED_COUNT,
  SCREEN_COUNT
};

// THE SELECTABLE VOCABULARY. Bounded by SCREEN_NAMED_COUNT, and that bound is
// what makes "there is no `screen pair`" true rather than a convention — the
// `screen` action's parse loop iterates THIS array.
const char *SCREEN_NAME[SCREEN_NAMED_COUNT] = {"status", "diag"};

// WHAT IS ON THE GLASS, which is a different question and used not to be
// answerable. `display status` reported SCREEN_NAME[screen_] — the user's
// SELECTION — so during pairing, with the pair screen actually being drawn, it
// said "status". Anyone verifying this device over the console was told the
// wrong thing about the one screen that matters most.
//
// SEPARATE ARRAY, NOT A LONGER SCREEN_NAME, and the separation is the point:
// extending SCREEN_NAME to SCREEN_COUNT would put "pair" inside the loop the
// `screen` action searches, and `screen pair` would start working. That must
// not regress — the QR IS the PIN in a form a camera reads from across a room,
// so an action that puts it on the panel is an action that leaks the pairing
// secret to anyone who can send one. Reporting a state and selecting it are
// different capabilities and now use different tables.
const char *ACTIVE_SCREEN_NAME[SCREEN_COUNT] = {"status", "diag", "pair"};
static_assert(SCREEN_NAMED_COUNT < SCREEN_COUNT, "the pair screen must stay outside the selectable range");

// Status and diag share one table — that is the "the two screens cost no
// extra" property, and it survives: what changed is that a THIRD screen now
// exists which could not share it.
constexpr const Rect *SCREEN_RECT[SCREEN_COUNT] = {STATUS_RECT, STATUS_RECT, PAIR_RECT};

// SELECTED -> SHOWING, in one place. displayTick() decides what goes on the
// glass with this, and the `screen` action answers with it, so a reply can
// never contradict the panel it is describing. Extracted when `display status`
// stopped reporting the selection and started reporting the truth: two copies
// of this expression is two things to keep in step, and the whole defect being
// fixed was two things that were not.
uint8_t effectiveScreen(uint8_t selected) {
  return (selected == SCREEN_STATUS && Pairing::visible()) ? (uint8_t)SCREEN_PAIR : selected;
}

const Rect &rectFor(uint8_t screen, uint8_t region) {
  return SCREEN_RECT[screen < SCREEN_COUNT ? screen : 0][region < REGION_COUNT ? region : 0];
}

// ===========================================================================
// State
// ===========================================================================

Adafruit_ST7735 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);

bool up_ = false;          // panel initialised and being driven
uint8_t blPct_ = 0;        // backlight 0..100
// SELECTED vs SHOWING, and they are not the same thing since the pairing
// screen landed. screen_ is what the operator chose and is always one of the
// NAMED screens; activeScreen_ is what is on the glass this tick and may be
// SCREEN_PAIR, which nobody can choose. displayTick derives the second from
// the first — every rect lookup, build and draw goes through activeScreen_,
// and `screen`/`status` report screen_.
uint8_t screen_ = SCREEN_STATUS;
uint8_t activeScreen_ = SCREEN_STATUS;
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
  // LIVE and INTENT, kept apart: hidArmed && !hidLive is "armed, binds at the
  // next boot", which is a different badge (backlog C3). See ScreenFmt::hidBadge.
  bool hidLive;
  bool hidArmed;
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
// WHAT THE WORST TICK CONTAINED. worstRenderUs_ on its own says a tick was
// slow; on a screen with four different expensive things in it that is not
// enough to act on, and guessing the decomposition from arithmetic was tried
// and got it wrong. Bit 0 full clear, bit 1 QR encode, bit 2 QR blit, bit 3
// world sample, high nibble the region count drawn.
constexpr uint8_t TICK_CLEAR = 0x01;
constexpr uint8_t TICK_ENCODE = 0x02;
constexpr uint8_t TICK_BLIT = 0x04;
constexpr uint8_t TICK_SAMPLE = 0x08;
uint8_t tickWhat_ = 0;
uint8_t worstRenderWhat_ = 0;
// The QR block, split into its two halves, for the same reason worstRenderUs_
// exists: this is the one region that can plausibly eat the tick budget, and
// "it got slow" is not actionable without knowing WHICH half got slow. The
// encode is CPU (mask selection is the expensive part); the blit is SPI.
uint32_t qrEncodeUs_ = 0;
uint32_t qrBlitUs_ = 0;

// ---- the encoded pairing QR --------------------------------------------
//
// THE SYMBOL IS CACHED, AND THE MEASUREMENT IS WHY. Encoding inside drawQr()
// was tried first, because it kept the code out of .bss entirely; on the
// device it cost 12,237 us per draw against 5,241 us for the blit, and a draw
// happens on every full clear (enable, `refresh`, screen change) as well as on
// every payload change. Paying 12 ms of mask selection to repaint pixels that
// had not changed is not defensible, so the symbol is now built once per
// Pairing::seq() and repainted from the buffer.
//
// The hygiene argument for NOT caching turned out to be thin: pairing.h
// already holds the same secret in .bss as PLAIN TEXT for exactly as long,
// which is strictly more readable than a masked QR bitstream. So this adds no
// new exposure class — but it does add a second thing that must be wiped, and
// qrForget() is that. It is called wherever the pair screen goes away.
uint8_t qrCode_[QrFit::BUF_LEN] = {0};
QrFit::Fit qrFit_ = {false, 0, 0, 0, 0};
uint32_t qrCodedSeq_ = 0;  // the Pairing::seq() qrCode_ was built from
bool qrCoded_ = false;     // qrCode_/qrFit_ describe the current record
// Set by ensureQrEncoded() when it actually did the ~12 ms of work, consumed by
// displayTick to yield the rest of that tick. See the comment there.
bool qrJustEncoded_ = false;

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
  // live, or one boot away from live, is not something to render as the fourth
  // word on a grey line.
  size_t used = 0;
  for (uint8_t i = 0; i < registry.count(); i++) {
    const ModuleDescriptor *m = registry.at(i);
    if (m == nullptr || m->id == nullptr) {
      continue;
    }
    // `hid` is read BEFORE the enabled filter, not after: an armed-but-unbound
    // hid is NOT enabled (the registry only records the intent until the next
    // boot), so the old "skip everything disabled" loop could never see the
    // state this badge exists to show.
    if (strcmp(m->id, "hid") == 0) {
      w.hidLive = registry.enabledAt(i);
      w.hidArmed = registry.armedAt(i);
      continue;
    }
    if (!registry.enabledAt(i)) {
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
//
// AUX_AUTH_PIN (== 1) used to sit between these two and is gone with the
// status screen's PIN branch — see buildStatus's REG_AUTH. The values of the
// survivors are deliberately NOT renumbered: aux is compared against the
// previous tick's cached value, never persisted or sent anywhere, so a gap
// costs nothing and renumbering would be a change with no reader.
constexpr uint32_t AUX_AUTH_BLANK = 0;
constexpr uint32_t AUX_AUTH_PAIRED = 2;
// REG_MODS's aux is a ScreenFmt::HidBadge (screenfmt.h) — OFF / LIVE / ARMED.
// It is in the cache key, so arming `hid` from a phone dirties the region and
// the badge appears within one tick, without a full redraw.

uint8_t animFrame(uint32_t now) { return (uint8_t)((now / ANIM_MS) & 3u); }

// Column budgets, derived rather than written down twice. Every one of these
// is (available pixels) / (character cell), so changing a margin or a font
// size cannot leave a literal behind that lets a string run off the panel.
constexpr size_t COLS_X2 = (size_t)((W - 2) / CHAR_W);          // a line starting at x=2
constexpr size_t COLS_ID = COLS_X2 - 3;                         // less the "AP" badge on the right
constexpr size_t COLS_MODS_HID = COLS_X2 - 4;                   // less the HID badge on the right
constexpr size_t COLS_ACT = (size_t)((W - 34) / CHAR_W);        // right of the chevrons

// Pair screen. The right-hand column starts 3 px in from the QR block, which
// leaves 12 columns — exactly the width of "tdongle-a9d8", the SSID this
// device generates, and the reason the margin is 3 rather than 2 or 4.
constexpr int16_t PAIR_TEXT_X = PAIR_COL_X + 3;
constexpr size_t COLS_PAIR = (size_t)((PAIR_COL_W - 3) / CHAR_W);
static_assert(COLS_PAIR == 12, "the pair column's text budget moved; re-check every string on that screen");
// The title bar gives up the same 22 px the status screen's REG_MODS does, to
// the same HID badge. See buildPair's REG_ID for why that badge follows the
// user onto this screen instead of being dropped with the module list.
constexpr size_t COLS_PAIR_TITLE = (size_t)((PAIR_COL_W - 3 - 22) / CHAR_W);
static_assert(COLS_PAIR_TITLE >= 9, "the pair title no longer fits beside the HID badge");
// The digits, at PAIR_PIN_SIZE. Derived the same way as everything else here
// so that raising the text size cannot leave a stale column count behind.
constexpr size_t COLS_PAIR_PIN = (size_t)(PAIR_COL_W / (CHAR_W * PAIR_PIN_SIZE));
static_assert(COLS_PAIR_PIN >= 4, "the pair screen can no longer show a 4-digit PIN");
// The text fallback inside the QR block, 2 px in on each side.
constexpr size_t COLS_QR_TEXT = (size_t)((QR_BLOCK - 4) / CHAR_W);
constexpr uint8_t LINES_QR_TEXT = (uint8_t)((H - 6) / CHAR_H);
// Two of those lines go to the "key:" label and the gap under it, so the
// budget that has to hold AuthFmt::PSK_MAX is the remainder — assert what is
// actually passed to drawWrapped, not the whole block.
constexpr uint8_t LINES_QR_KEY = (uint8_t)(LINES_QR_TEXT - 2);
static_assert(COLS_QR_TEXT * LINES_QR_KEY >= 63,
              "the fallback block can no longer hold a 63-character passphrase");

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
      //
      // THAT PROPERTY NOW COVERS THE QR TOO, and it had to: a QR is the PIN in
      // a form a camera reads faster than a person. It is enforced in
      // displayTick, where SCREEN_PAIR is only selected while
      // screen_ == SCREEN_STATUS — read the comment there before changing
      // either condition.
      //
      // WHAT USED TO BE HERE AND WHY IT IS GONE. This branch drew the PIN at
      // text size 2 in an 18 px stripe. It is now UNREACHABLE, and deleting it
      // rather than leaving it as a fallback is deliberate: displayTick
      // switches the whole panel to SCREEN_PAIR whenever
      // (screen_ == SCREEN_STATUS && Pairing::visible()), which is exactly the
      // condition this branch tested. The two remaining cases are the ones
      // below — and `diag`, which reaches buildDiag instead and shows neither.
      //
      // The digits themselves did not disappear; they moved to buildPair at
      // size 3, which is the legibility the 8->4 digit change was for.
      if (world_.apUp && world_.sessions > 0) {
        boundedf(out, cap, COLS_X2, "paired  %u session%s", (unsigned)world_.sessions,
                 world_.sessions == 1 ? "" : "s");
        *aux = AUX_AUTH_PAIRED;
      } else {
        *aux = AUX_AUTH_BLANK;
      }
      break;
    }

    case REG_MODS: {
      // Both badge states occupy the SAME 22 px on the right, so the text
      // budget is the same for either and COLS_MODS_HID stays the one number
      // that has to agree with the drawing code below.
      ScreenFmt::HidBadge badge = ScreenFmt::hidBadge(world_.hidArmed, world_.hidLive);
      size_t cols = (badge == ScreenFmt::HID_OFF) ? COLS_X2 : COLS_MODS_HID;
      ScreenFmt::fitPrefixed(out, cap, "on: ", world_.mods, cols);
      *aux = (uint32_t)badge;
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

// Drop the encoded symbol. Called wherever the pair screen goes away, so the
// machine-readable copy of the PIN (or the passphrase) does not outlive the
// screen that needed it — the same discipline pairing.h applies to the text.
void qrForget() {
  memset(qrCode_, 0, sizeof(qrCode_));
  qrFit_ = QrFit::Fit{false, 0, 0, 0, 0};
  qrCoded_ = false;
  qrCodedSeq_ = 0;
}

// Build the symbol for the current record, if it is not already built. Costs
// ~12 ms on this chip (measured, qr_encode_us) and therefore runs exactly once
// per Pairing::seq() rather than once per repaint.
void ensureQrEncoded(uint32_t seq) {
  if (qrCoded_ && qrCodedSeq_ == seq) {
    return;
  }
  qrForget();
  qrCodedSeq_ = seq;
  qrCoded_ = true;  // "we have tried"; qrFit_.ok says whether it worked

  Pairing::Snapshot s;
  if (Pairing::get(s)) {
    // Scratch is stack; only the finished symbol is kept.
    uint8_t tmp[QrFit::BUF_LEN];
    uint32_t t0 = micros();
    qrFit_ = QrFit::encode(s.payload, tmp, qrCode_);
    qrEncodeUs_ = micros() - t0;
    qrJustEncoded_ = true;
    tickWhat_ |= TICK_ENCODE;
    memset(tmp, 0, sizeof(tmp));
  }
  memset(&s, 0, sizeof(s));
}

// ---- the pairing screen -------------------------------------------------
//
// THIS FUNCTION HOLDS NO POLICY ABOUT WHICH CODE IS SHOWING. Pairing::code()
// is read and rendered; the decision between JOIN and PAIR belongs to
// mod_http.cpp, which owns the association count and both secrets. Keeping
// that division is the whole reason this module can stay a renderer — see the
// block comment in pairing.h.
void buildPair(uint8_t region, char *out, size_t cap, uint32_t *aux) {
  const Pairing::Code code = Pairing::code();
  // The code is in every aux on this screen: it changes the words, and a
  // JOIN->PAIR switch has to repaint the column even when the PIN did not move.
  *aux = (uint32_t)code;

  switch (region) {
    case REG_ID: {
      // An imperative, because this screen arrives unprompted in front of
      // someone who did not ask for it and has a phone in their hand. Both
      // strings are 9 columns or fewer, which is what COLS_PAIR_TITLE leaves
      // once the badge has its 22 px.
      //
      // THE HID BADGE FOLLOWS THE USER HERE, and that is not decoration. This
      // screen owns the whole panel for as long as the device is pairable —
      // potentially hours, sitting in the front of a machine — and BRIEF.md
      // section 5 asks that "is this thing currently a keyboard?" never
      // require navigation to answer. Dropping the module list on an 80 px
      // column is fine; dropping the one indicator that says keystrokes can be
      // injected into the host PC right now is not.
      ScreenFmt::HidBadge badge = ScreenFmt::hidBadge(world_.hidArmed, world_.hidLive);
      ScreenFmt::fit(out, cap, code == Pairing::CODE_JOIN ? "JOIN WIFI" : "PAIR NOW", COLS_PAIR_TITLE);
      *aux = (uint32_t)code | ((uint32_t)badge << 8);
      break;
    }

    case REG_WIFI:
      if (code == Pairing::CODE_JOIN) {
        // THE SSID, because the user is about to pick this network out of a
        // list of every AP in the building. Falls back to devName_ (the same
        // MAC-derived string, built here from the eFuse) if `http` has not
        // reported yet, so this line is never blank at the moment it matters.
        ScreenFmt::fit(out, cap, world_.ssid[0] != '\0' ? world_.ssid : devName_, COLS_PAIR);
      } else {
        // Already joined. The address is what a user needs if the camera is
        // the thing that is not working.
        ScreenFmt::fit(out, cap, world_.ip[0] != '\0' ? world_.ip : "192.168.4.1", COLS_PAIR);
      }
      break;

    case REG_AUTH: {
      // The PIN, large. Read through the Snapshot rather than a narrower
      // accessor so there is exactly one way into this header (pairing.h's
      // THREADING note), and wiped off the stack on the way out.
      Pairing::Snapshot s;
      if (Pairing::get(s)) {
        ScreenFmt::fit(out, cap, s.pin, COLS_PAIR_PIN);
      }
      memset(&s, 0, sizeof(s));
      break;
    }

    case REG_NET:
      ScreenFmt::fit(out, cap, code == Pairing::CODE_JOIN ? "scan to join" : "scan or type", COLS_PAIR);
      break;

    default:
      break;
  }
}

void buildRegion(uint8_t region, uint32_t now, char *out, size_t cap, uint32_t *aux) {
  out[0] = '\0';
  *aux = 0;

  // A region that does not exist on this screen builds nothing, so it never
  // goes dirty and never consumes one of the MAX_DRAW_PER_TICK slots. Without
  // this the footer's animation frame would dirty REG_FOOT eight times a
  // second on the pair screen, where REG_FOOT is NO_RECT and draws nothing.
  if (rectEmpty(rectFor(activeScreen_, region))) {
    return;
  }

  if (region == REG_QR) {
    // The QR's cache key is Pairing::seq() and NOTHING ELSE. The payload is up
    // to 224 bytes and is a secret (it carries either the passphrase or the
    // PIN); copying it into cache_[].text would be a third copy of it in RAM,
    // and cache_[].text is 48 bytes anyway. seq() moves on every real change
    // to the record, which is exactly when the symbol has to be rebuilt.
    *aux = Pairing::seq();
    // Encoding happens HERE, not in drawQr, and this is still "no drawing in
    // build": it is pure CPU over a payload, touching no SPI. Doing it on the
    // cache pass rather than the draw pass is what stops a repaint of
    // unchanged pixels from paying for mask selection again.
    ensureQrEncoded(*aux);
    return;
  }
  if (activeScreen_ == SCREEN_PAIR) {
    buildPair(region, out, cap, aux);
    return;
  }
  if (region == REG_FOOT) {
    buildFoot(now, out, cap, aux);
    return;
  }
  if (activeScreen_ == SCREEN_DIAG) {
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

// Chunk-wrap, not word-wrap. The only string this ever draws is a WPA2
// passphrase, which has no words to break on, and breaking one at a space
// would hide whether the space is part of it (0x20 is a legal passphrase
// character — authfmt.h).
void drawWrapped(int16_t x, int16_t y, const char *s, uint16_t colour, size_t cols, uint8_t maxLines) {
  char line[32];
  size_t n = strlen(s);
  for (uint8_t ln = 0; ln < maxLines; ln++) {
    size_t off = (size_t)ln * cols;
    if (off >= n) {
      break;
    }
    size_t take = n - off;
    if (take > cols) {
      take = cols;
    }
    if (take > sizeof(line) - 1) {
      take = sizeof(line) - 1;
    }
    memcpy(line, s + off, take);
    line[take] = '\0';
    drawText(x, (int16_t)(y + (int16_t)ln * CHAR_H), line, colour, 1);
  }
  memset(line, 0, sizeof(line));  // it was a passphrase
}

// ===========================================================================
// The pairing QR block
// ===========================================================================
//
// ---- THE DRAWING BUDGET, MEASURED RATHER THAN ASSUMED ------------------
//
// MAX_DRAW_PER_TICK exists to bound SPI per tick, and the comment above it
// prices two 160x18 stripes at ~5,760 bytes / ~1.2 ms at 40 MHz. This block is
// 80x80 = 6,400 pixels = 12,800 bytes, which on paper is ~2.56 ms — already
// enough to blow that budget in ONE region.
//
// ON PAPER WAS WRONG, AND BY A LOT. The first working version encoded and drew
// inside this function, and the device reported:
//
//     qr_encode_us  12,237      qrcodegen_encodeText, mask AUTO, version 1
//     qr_blit_us     5,241      80 writePixels() calls, 12,800 bytes
//     worst_render_us 17,499    i.e. THE QR REGION WAS THE WORST TICK
//
// 17.5 ms of a 40 ms tick, on every repaint, is not a bounded one-off — it is
// 44% of the interval every time the screen is cleared. Two things were wrong
// and both are fixed rather than justified:
//
//   * THE ENCODE WAS PAID PER DRAW. It is a pure function of the payload, so
//     it now happens once per Pairing::seq() in ensureQrEncoded() and a
//     repaint of unchanged pixels pays nothing for it.
//   * THE BLIT WAS ASSUMED TO BE TWICE ITS FLOOR — 5,241 us against 2,560 us
//     of transfer at 40 MHz — and that gap was attributed to per-transaction
//     overhead, 80 times over. Rows were batched into bands to close it.
//     THAT DIAGNOSIS WAS WRONG. See the measurement in drawQr() below: the
//     banding made it SLOWER, and the missing time is the 6,400
//     qrcodegen_getModule() calls, not the SPI.
//
// What is left, and it is stated as a cost rather than hidden: the tick on
// which the PIN rotates still pays encode + blit together. Nothing here splits
// those across ticks — that would need sub-region state the dirty tracker does
// not have — and a PIN rotation is minutes apart, so it is one slow tick in a
// cooperative 40 ms schedule rather than a steady-state load. The evidence is
// qr_encode_us / qr_blit_us / worst_render_us on `display status`, not this
// comment.
//
// THE MASK IS DELIBERATELY LEFT ON AUTO. qrcodegen_Mask_AUTO is most of the
// 12 ms: it encodes eight times and scores each for decoder-hostile patterns.
// Forcing one mask would cut that by roughly 8x and is the obvious next lever
// — and it is NOT taken here, because mask choice affects how a real camera
// copes with a real symbol on real glass, and nothing on this machine can test
// that. It is stuart's call, after scanning.
//
// NOT fillRect PER MODULE. A 29x29 code is 841 rectangles, i.e. 841 address
// windows and 841 transactions to move 12,800 bytes.
void drawQr(const Rect &q) {
  if (qrCoded_ && qrFit_.ok) {
    const uint32_t t0 = micros();
    // ONE ADDRESS WINDOW over the whole block; every pixel of it is written,
    // so the quiet zone paints itself and there is no separate fill. Module
    // lookup for a pixel outside the symbol yields false — qrcodegen_getModule
    // bounds-checks — which is what makes that work.
    //
    // ROWS ARE BATCHED, AND IT BOUGHT NOTHING. This comment used to claim that
    // one writePixels() per row cost 5,241 us against a 2,560 us transfer floor
    // and that banding "cuts it". MEASURED ON THE DEVICE, 2026-08-24, both
    // arms built from this same source and flashed back to back, six
    // `display refresh` repaints each, reading qr_blit_us off `display status`:
    //
    //     QR_ROWS_PER_BAND = 1  (80 writePixels calls)   5,478..5,494 us
    //     QR_ROWS_PER_BAND = 8  (10 writePixels calls)   5,622..5,679 us
    //
    // Banding is about 175 us SLOWER — 3% the wrong way — and costs 1,280 bytes
    // of stack for the band buffer. The blit writes all 6,400 pixels regardless
    // of the symbol's version, so the two arms are like for like.
    //
    // THE COST IS THE MODULE LOOKUP, NOT THE TRANSPORT. 70 SPI transactions'
    // difference is worth ~175 us, i.e. ~2.5 us per call — trivial. What is
    // left is 6,400 qrcodegen_getModule() calls, each bounds-checking and
    // extracting a bit, at roughly 0.8 us apiece. Anything that makes this
    // faster has to attack that loop: at scale 2 each module covers a 2x2 pixel
    // block, so 841 lookups would do the work of 6,400.
    //
    // NOT DONE HERE, deliberately. It is a real change to code that puts a
    // machine-readable secret on glass, and this machine has no camera — a
    // regression that smears or mirrors the symbol cannot be detected from
    // here, only by stuart pointing a phone at the panel. It is also not worth
    // much: the blit is 5.6 ms of a 40 ms tick, it happens on repaints minutes
    // apart, and the encode beside it is 24 ms. Dropping the banding back to 1
    // row is the free half of it and is stuart's call, not a quiet edit —
    // the numbers above are here so he can make it.
    tft.startWrite();
    tft.setAddrWindow((uint16_t)q.x, (uint16_t)q.y, (uint16_t)q.w, (uint16_t)q.h);
    uint16_t band[QR_BLOCK * QR_ROWS_PER_BAND];
    int16_t filled = 0;
    for (int16_t y = 0; y < q.h; y++) {
      const int my = (y - (int16_t)qrFit_.offset) / qrFit_.scale - QrFit::QUIET_MODULES;
      uint16_t *row = band + (size_t)filled * (size_t)q.w;
      for (int16_t x = 0; x < q.w; x++) {
        const int mx = (x - (int16_t)qrFit_.offset) / qrFit_.scale - QrFit::QUIET_MODULES;
        row[x] = qrcodegen_getModule(qrCode_, mx, my) ? C_QR_DARK : C_QR_LIGHT;
      }
      filled++;
      // Flush on a full band, and on the last row whether or not it is full —
      // 80 is a multiple of 8 today, but that is layout, not a law.
      if (filled == QR_ROWS_PER_BAND || y == q.h - 1) {
        tft.writePixels(band, (uint32_t)filled * (uint32_t)q.w);
        filled = 0;
      }
    }
    tft.endWrite();
    qrBlitUs_ = micros() - t0;
    tickWhat_ |= TICK_BLIT;
    return;
  }

  // THE FALLBACK, AND IT IS A REAL CASE. qrfit.h explains which payload reaches
  // it: a `WIFI:` join code carrying a 63-character owner-set passphrase needs
  // version 5, which would be 1 px per module — 0.14 mm, below what any phone
  // can resolve. An unreadable QR is worse than no QR because it looks like it
  // should work, so the block becomes text.
  //
  // WHAT THIS COSTS: 12 columns. The passphrase wraps into five short lines
  // instead of the ~26-column ones a full-width layout would give. Making it
  // full width needs a THIRD rect table selected on the fit result, and that
  // was judged not worth the machinery for a case only an owner-set 63-char
  // passphrase reaches — `psk` regenerates a 15-character one that encodes at
  // version 3.
  //
  // The Snapshot is taken ONLY on this path: the common path repaints from
  // qrCode_ and never puts the payload on the stack at all.
  tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
  Pairing::Snapshot s;
  // Relative to the region, not to the panel: this block is at x == 0 today,
  // but a hardcoded x is exactly the kind of thing that survives a layout
  // change and draws into the neighbouring column.
  const int16_t tx = (int16_t)(q.x + 2);
  if (Pairing::get(s) && s.fallback[0] != '\0') {
    drawText(tx, (int16_t)(q.y + 3), "key:", C_DIM, 1);
    drawWrapped(tx, (int16_t)(q.y + 3 + CHAR_H + 2), s.fallback, C_WARN, COLS_QR_TEXT, LINES_QR_KEY);
  } else {
    // No payload at all, or one that will not encode and no text to show
    // instead. Say so rather than leaving a black square that looks like a
    // panel fault.
    drawText(tx, (int16_t)(q.y + 3), "no QR", C_DIM, 1);
  }
  memset(&s, 0, sizeof(s));  // it held the passphrase
}

// The pair screen's own drawing. Same region IDs, different geometry and one
// much larger text size — see PAIR_PIN_SIZE for why the digits are size 3.
void drawPairRegion(uint8_t r, const Rect &q, const char *text, uint32_t aux) {
  const Pairing::Code code = (Pairing::Code)(aux & 0xFFu);

  switch (r) {
    case REG_ID: {
      tft.fillRect(q.x, q.y, q.w, q.h, C_BAR);
      drawText(PAIR_TEXT_X, (int16_t)(q.y + 2), text, C_WARN, 1);
      // Same badge, same 22 px, same two states as REG_MODS on the status
      // screen — solid red for LIVE, hollow yellow for ARMED. Kept identical
      // on purpose: a safety indicator that changes shape between screens is
      // a safety indicator nobody learns.
      ScreenFmt::HidBadge badge = (ScreenFmt::HidBadge)((aux >> 8) & 0xFFu);
      if (badge == ScreenFmt::HID_LIVE) {
        tft.fillRect((int16_t)(W - 22), q.y, 22, q.h, C_ALERT);
        drawText((int16_t)(W - 20), (int16_t)(q.y + 2), "HID", ST77XX_BLACK, 1);
      } else if (badge == ScreenFmt::HID_ARMED) {
        tft.drawRect((int16_t)(W - 22), q.y, 22, q.h, C_WARN);
        drawText((int16_t)(W - 20), (int16_t)(q.y + 2), "HID", C_WARN, 1);
      }
      break;
    }

    case REG_WIFI:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      drawText(PAIR_TEXT_X, (int16_t)(q.y + 2), text, code == Pairing::CODE_JOIN ? C_TEXT : C_INFO, 1);
      break;

    case REG_AUTH: {
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      // Vertically centred: an 8 px label, 2 px, then 24 px of digits is 34 px
      // of content in a 44 px region.
      constexpr int16_t content = CHAR_H + 2 + PAIR_PIN_SIZE * CHAR_H;
      const int16_t top = (int16_t)(q.y + (q.h - content) / 2);
      drawText(PAIR_TEXT_X, top, "PIN", C_DIM, 1);
      // Centred horizontally in the column at the drawn width, so a PIN that
      // is ever not 4 digits still sits under its label rather than running
      // off the edge.
      const int16_t pinW = (int16_t)(strlen(text) * CHAR_W * PAIR_PIN_SIZE);
      const int16_t pinX = (int16_t)(PAIR_COL_X + (PAIR_COL_W - pinW) / 2);
      drawText(pinX, (int16_t)(top + CHAR_H + 2), text, C_WARN, PAIR_PIN_SIZE);
      break;
    }

    case REG_NET:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      drawText(PAIR_TEXT_X, (int16_t)(q.y + 2), text, C_DIM, 1);
      break;

    default:
      break;
  }
}

void drawRegion(uint8_t r) {
  const Rect &q = rectFor(activeScreen_, r);
  const char *text = cache_[r].text;
  const uint32_t aux = cache_[r].aux;

  // Not on this screen. buildRegion already refuses to dirty it, so this is
  // belt and braces — but a stray Dirty::markAll() must not be able to make
  // fillRect paint a zero-width rect at the top-left corner.
  if (rectEmpty(q)) {
    return;
  }

  if (r == REG_QR) {
    drawQr(q);
    return;
  }
  if (activeScreen_ == SCREEN_PAIR) {
    drawPairRegion(r, q, text, aux);
    return;
  }

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
      // The size-2 PIN that used to be drawn here has moved to the pairing
      // screen at size 3 (drawPairRegion). The 18 px this region still gets is
      // now more than "paired  1 session" needs; it is left alone rather than
      // reclaimed, because shrinking it would move every boundary below it for
      // no visible gain.
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      if (aux == AUX_AUTH_PAIRED) {
        drawText(2, (int16_t)(q.y + 5), text, C_OK, 1);
      }
      break;

    case REG_MODS:
      tft.fillRect(q.x, q.y, q.w, q.h, C_BG);
      drawText(2, (int16_t)(q.y + 2), text, C_DIM, 1);
      // Badge geometry, once: x from W-22 to W-1 (22 px), the full region
      // height, text 2 px inside at size 1 — "HID" is 3 * CHAR_W = 18 px, so it
      // clears the right-hand edge by 2 px and the outlined variant's border by
      // 1 px on every side.
      if (aux == (uint32_t)ScreenFmt::HID_LIVE) {
        // SOLID RED. hid is live: keystrokes can be injected into the host PC
        // right now. ARCHITECTURE.md section 4 makes that reboot-gated and
        // never default-enabled; this is the outward sign that it happened.
        tft.fillRect((int16_t)(W - 22), q.y, 22, q.h, C_ALERT);
        drawText((int16_t)(W - 20), (int16_t)(q.y + 2), "HID", ST77XX_BLACK, 1);
      } else if (aux == (uint32_t)ScreenFmt::HID_ARMED) {
        // HOLLOW YELLOW. Armed but not bound: nothing can be typed until the
        // device is power-cycled, and then it can. Deliberately unlike BOTH
        // neighbours in the state space — not the absence of a badge, and not
        // the solid red block — because the difference that matters is "already
        // dangerous" versus "dangerous after the next reboot". Outline rather
        // than fill says the same thing in shape as well as colour, for anyone
        // reading it across a room or on a photograph.
        tft.drawRect((int16_t)(W - 22), q.y, 22, q.h, C_WARN);
        drawText((int16_t)(W - 20), (int16_t)(q.y + 2), "HID", C_WARN, 1);
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
  tickWhat_ = 0;

  if (lastSampleMs_ == 0 || (uint32_t)(now - lastSampleMs_) >= SAMPLE_MS) {
    lastSampleMs_ = now;
    sampleWorld();
    tickWhat_ |= TICK_SAMPLE;
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

  // ---- which screen is actually showing --------------------------------
  //
  // The pairing screen appears BY ITSELF while Pairing::visible() and vanishes
  // when the record is withdrawn. There is no `screen pair`, no action that
  // can request it, and no way to ask for it over the wire — because the QR IS
  // THE PIN, in a form a camera reads faster and from further away than a
  // person reads digits, and an action that puts it on the panel would be an
  // action that leaks the pairing secret to anyone who can send one.
  //
  // AND `diag` STILL HIDES IT. The condition is screen_ == SCREEN_STATUS, so
  // an operator who has selected `diag` sees diag — no QR, no digits. That is
  // the same property REG_AUTH has carried since the PIN first appeared here
  // and the reason is unchanged: hiding the secret is a deliberate operator
  // action, reversible in one command; the reverse is not.
  const uint8_t effective = effectiveScreen(screen_);
  if (effective != activeScreen_) {
    // Same handling as a `screen` command, and for the same reason: the region
    // geometry changes completely, so the panel must be blanked rather than
    // painted over. Without the cache reset, a region whose text happens to
    // match its previous screen's would never be marked dirty and would keep
    // the old screen's pixels.
    activeScreen_ = effective;
    clearPending_ = true;
    memset(cache_, 0, sizeof(cache_));
    if (effective != SCREEN_PAIR) {
      // Leaving the pair screen — because pairing withdrew, or because an
      // operator switched to `diag`. Either way the encoded symbol is a
      // machine-readable copy of a secret that nothing is going to draw, so it
      // goes now rather than sitting in .bss until the next pairing attempt.
      qrForget();
    }
  }

  refreshCache(now);

  // ---- yield the tick that just encoded a QR ----------------------------
  //
  // MEASURED, and the reason this branch exists at all. Encoding is ~12.2 ms
  // (qr_encode_us) and it happens in refreshCache above. Without this, the tick
  // that enters the pairing screen paid for the encode AND the full clear AND
  // the 5.4 ms blit in one go — 26,595 us of a 40 ms interval, worse than the
  // naive version this was supposed to improve on.
  //
  // So a tick that encoded does nothing else. Everything stays dirty and paints
  // 40 ms later, which is one frame and invisible; the peak drops to the clear
  // plus the blit. This is the "bands across ticks" idea from the design note,
  // applied at the seam that actually costs — between CPU and SPI — rather than
  // by slicing the symbol, which would have needed sub-region state the dirty
  // tracker does not have.
  if (qrJustEncoded_) {
    qrJustEncoded_ = false;
  } else {
    if (clearPending_) {
      // Full repaint: one 25,600-byte transfer, ~10 ms measured on this panel
      // (the ~5.1 ms this comment used to claim was the transfer time alone and
      // ignored per-pixel cost). It happens on enable, on `refresh` and on a
      // screen change — never on a state change — so it is a bounded one-off
      // rather than something in the steady-state path. All seven regions then
      // repaint over the next four ticks (REGION_COUNT / MAX_DRAW_PER_TICK),
      // i.e. 160 ms — one more tick than before, since REG_QR joined the set.
      tft.fillScreen(C_BG);
      tickWhat_ |= TICK_CLEAR;
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
    tickWhat_ |= (uint8_t)(drawn << 4);
  }

  frames_++;
  lastRenderUs_ = micros() - t0;
  if (lastRenderUs_ > worstRenderUs_) {
    worstRenderUs_ = lastRenderUs_;
    worstRenderWhat_ = tickWhat_;
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
  qrForget();
  Dirty::init(dirty_, REGION_COUNT);
  Dirty::markAll(dirty_);
  lastSampleMs_ = 0;
  clearPending_ = false;
  screen_ = SCREEN_STATUS;
  activeScreen_ = SCREEN_STATUS;

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
  Pairing::subscribe(false);  // also wipes the PIN and the QR payload out of pairing.h's buffers
  qrForget();                 // and the encoded symbol out of ours

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
  (void)ctx;  // every action of this module is gated centrally (modauth.h)

  if (strcmp(act, "status") == 0) {
    d["panel"] = up_;
    // WHAT IS BEING DRAWN, not what was asked for. These differ for exactly one
    // state — pairing — and that state is the one an operator most needs the
    // truth about. `screen_selected` keeps the old value available, because
    // "the operator chose diag and pairing is therefore hidden" and "the
    // operator chose status and pairing is not showing" are different
    // situations that `screen` alone can no longer distinguish.
    d["screen"] = ACTIVE_SCREEN_NAME[activeScreen_ < SCREEN_COUNT ? activeScreen_ : 0];
    d["screen_selected"] = SCREEN_NAME[screen_ < SCREEN_NAMED_COUNT ? screen_ : 0];
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
    d["worst_render_what"] = worstRenderWhat_;
    d["qr_encode_us"] = qrEncodeUs_;
    d["qr_blit_us"] = qrBlitUs_;
    // ---- WHICH SYMBOL IS ON THE GLASS -----------------------------------
    //
    // ADDED BECAUSE THE VERIFICATION OF THIS FEATURE WAS AN ARGUMENT RATHER
    // THAN A MEASUREMENT. With only qr_encode_us and qr_blit_us to go on,
    // "the panel is showing the JOIN code" had to be inferred from a timing
    // ratio — 24,377 us against 12,237 us for a known version-1 encode, i.e.
    // 1.99, which excludes version 1 and points at version 3. That is a good
    // inference and it is not a fact. These three make it one.
    //
    // THEY DISCLOSE NOTHING. A symbol's version is its SIZE, not its content:
    // "29 modules" is derivable from the passphrase's LENGTH, which `psk`
    // already reports (at AUTH_PHYSICAL) and which the panel itself shows to
    // anyone who can see it. `qr_code` says JOIN or PAIR, which is the same
    // bit `http status`'s `station_associated` already carries at this same
    // auth level. The payload, the PIN and the passphrase remain unreachable
    // from every transport — see the SECRETS block in mod_http.cpp.
    //
    // qr_ok false with a code published means THE TEXT FALLBACK IS ON THE
    // GLASS: the payload encoded too large to draw at 2 px/module (qrfit.h),
    // so the panel is printing the passphrase instead of a symbol. That is
    // otherwise invisible from here, and it is the state an owner-set
    // passphrase over ~23 characters silently puts the device into.
    d["qr_code"] = Pairing::code() == Pairing::CODE_JOIN   ? "join"
                   : Pairing::code() == Pairing::CODE_PAIR ? "pair"
                                                           : "none";
    d["qr_ok"] = qrCoded_ && qrFit_.ok;
    // qrfit.h's sizeForVersion() inverted: 21, 25, 29 modules are versions 1..3.
    d["qr_version"] = (uint32_t)(qrCoded_ && qrFit_.ok ? (qrFit_.size - 17) / 4 : 0);
    d["qr_size"] = (uint32_t)(qrCoded_ && qrFit_.ok ? qrFit_.size : 0);
    d["qr_scale"] = (uint32_t)(qrCoded_ && qrFit_.ok ? qrFit_.scale : 0);
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
    // The hand-rolled AUTH_TOKEN check that used to be here is gone (backlog
    // S6): the level is declared in modauth.h and Registry::dispatch() applies
    // it. Its reasoning still stands and is why the whole module is TOKEN — a
    // device whose screen an unauthenticated client can blank cannot be trusted
    // as the out-of-band channel that shows the PIN. What has CHANGED is that
    // `screen` and `refresh` are now gated too (backlog C7): switching to
    // `diag` hides the PIN, which is an unauthenticated state change to that
    // same channel, and the old comment here claimed the opposite.
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
    // SCREEN_NAMED_COUNT, not SCREEN_COUNT: SCREEN_PAIR sits above it and is
    // firmware-selected. This loop IS the vocabulary of the action, so the
    // bound is what makes "there is no `screen pair`" true rather than a
    // convention.
    for (uint8_t i = 0; i < SCREEN_NAMED_COUNT; i++) {
      if (strcmp(name, SCREEN_NAME[i]) == 0) {
        if (screen_ != i) {
          screen_ = i;
          clearPending_ = true;
          memset(cache_, 0, sizeof(cache_));
        }
        d["screen_selected"] = SCREEN_NAME[i];
        // WHAT WILL ACTUALLY BE DRAWN, from the same expression displayTick()
        // applies (within one 40 ms tick). Selecting "status" while pairing is
        // visible puts the PAIR screen on the glass, and a reply that said
        // "status" would be the same lie `display status` used to tell. The
        // action's vocabulary is unchanged — this is a report, not a selection,
        // and "pair" still cannot be asked for.
        d["screen"] = ACTIVE_SCREEN_NAME[effectiveScreen(i)];
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
  // The SAME field name as `display status` and now the same meaning: what is
  // on the glass. It said the selected screen here too, so `modules` and the
  // panel disagreed during pairing exactly as the action did.
  d["screen"] = ACTIVE_SCREEN_NAME[activeScreen_ < SCREEN_COUNT ? activeScreen_ : 0];
  d["screen_selected"] = SCREEN_NAME[screen_ < SCREEN_NAMED_COUNT ? screen_ : 0];
  d["backlight_pct"] = blPct_;
  d["frames"] = frames_;
  d["region_writes"] = regionWrites_;
  d["worst_render_us"] = worstRenderUs_;
  d["pin_on_screen"] = Pairing::visible();
}

// Read off displayDispatch above. Both `backlight` parameters are optional
// because either one alone satisfies it — but at least one is required, and
// pct is checked first, so `on` is ignored when both are sent. The help says
// so; the schema does not invent a mutual exclusion the dispatch never
// enforces.
const ModuleParam BACKLIGHT_PARAMS[] = {
    ModParam::num("pct", false, "brightness 0..100 (0 == off). Wins if `on` is sent too. One of pct/on is required.", 0,
                  100),
    ModParam::flag("on", false, "true == full brightness, false == off. Ignored when pct is given. One of pct/on is required."),
};

const ModuleParam SCREEN_PARAMS[] = {
    ModParam::choice("name", true,
                     "which firmware-defined screen to show. `diag` also HIDES the pairing PIN and its QR code. "
                     "The pairing screen itself is not selectable — it appears on its own while the device is pairable.",
                     "status,diag"),
};

constexpr ModuleAction DISPLAY_ACTIONS[] = {
    {"status", "panel, backlight, current screen and redraw statistics", nullptr, 0,
     ModAuth::requiredFor("display", "status")},
    // The "(auth >= token)" that used to be in this help string is gone: the
    // level is emitted as `min_auth` by Registry::list() now, so a UI reads it
    // as data instead of parsing it out of English.
    {"backlight", "backlight on/off or 0..100%", MOD_PARAMS(BACKLIGHT_PARAMS),
     ModAuth::requiredFor("display", "backlight")},
    {"screen", "select a firmware-defined screen", MOD_PARAMS(SCREEN_PARAMS),
     ModAuth::requiredFor("display", "screen")},
    {"refresh", "force a full repaint", nullptr, 0, ModAuth::requiredFor("display", "refresh")},
};
static_assert(ModAuth::allGated(DISPLAY_ACTIONS, sizeof(DISPLAY_ACTIONS) / sizeof(DISPLAY_ACTIONS[0])),
              "display: an action has no declared auth level");
static_assert(ModAuth::isModuleListed("display"), "display has no row in ModAuth::MODULES");

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
    .minAuth = ModAuth::moduleMinimum("display"),
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
