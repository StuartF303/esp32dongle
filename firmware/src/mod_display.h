// usbdongle W3 — `display`: the 160x80 ST7735 status screen.
//
// See ARCHITECTURE.md section 4. Claims RES_LCD EXCLUSIVE; nothing else on the
// board wants SPI2_HOST today, so the claim is about the panel, not the bus.
//
// ---- THE SCREEN IS DEVICE-OWNED. THAT IS THE POINT. ---------------------
//
// Stuart's decision, 2026-08-17, and it is a security property rather than a
// scoping one. There is NO action here that draws caller-supplied content —
// no `text`, no `draw`, no `image`, and no plan for one. Every pixel comes
// from firmware reading live device state.
//
// The reason is ARCHITECTURE.md section 4's auth model: the LCD is the
// out-of-band channel that lets someone standing at the device read a pairing
// PIN that never crossed the network. A channel a client can write to is not
// out-of-band any more — it is a second display surface for whoever already
// has a session, and the PIN on it becomes as trustworthy as the phone that
// asked for it. So the actions here select among FIRMWARE-DEFINED screens and
// control the backlight, and that is all they will ever do.
//
// The one thing other code may put on this screen is progress, through
// activity.h, and even that is a verb and a percentage rendered by this
// module's own code.
//
// ---- NO FRAMEBUFFER -----------------------------------------------------
//
// Direct draw. 160 x 80 x 16 bpp is 25,600 bytes, and this chip has no PSRAM;
// ARCHITECTURE.md section 5 measures ~308 KB free heap with TinyUSB, before
// Wi-Fi (~50 KB with the AP up) and NimBLE (~40 KB, still to come). A quarter
// of a megabyte of headroom does not stretch to a framebuffer for a screen
// that renders six lines of text.

#pragma once

#include "registry.h"

// Static descriptor; register with registry.add() from setup().
const ModuleDescriptor *displayModuleDescriptor();
