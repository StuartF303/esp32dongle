# Design brief — T-Dongle-S3 control UI

You are designing the phone/tablet interface for a piece of hardware. This document is the
handoff; `api-samples/` holds real payloads captured from the running device, not mock-ups.

Read `HANDBACK.md` for what to deliver and in what form.

---

## 1. What the thing is

A **LilyGO T-Dongle-S3** — a USB stick, roughly the size of a car key fob, with a 160×80 colour
LCD on one face and a microSD slot hidden inside the USB-A shell. It plugs into a computer.

It runs custom firmware that turns it into several tools at once:

- a **USB keyboard** that types on the host computer on command (macro pad / automation)
- a **file bridge** to the microSD card and to 7.75 MB of internal storage
- a **Wi-Fi access point** serving the UI you are designing
- a **status display** on its own little screen

You control it from a phone over its own Wi-Fi network. **There is no internet connection while
you are connected to it** — the dongle's AP is not a route to anywhere. This is the single most
important constraint on your design.

### The user

One person: Stuart, in Edinburgh. A C#/.NET developer who also does electronics, CNC machining
and welding. He is technical and reads the code. He is not a novice who needs hand-holding, but
he *is* often using this one-handed, in a workshop, on a phone, while the other hand is busy.

This is not a consumer product. It does not need onboarding wizards or marketing polish. It needs
to be **fast to operate, unambiguous about dangerous actions, and legible at arm's length**.

---

## 2. The central design problem

**The UI is generated from data the device sends, not hardcoded per feature.**

The firmware exposes a list of *modules* (`hid`, `storage`, `display`, `http`, `led`, `cdc`). Each
module describes itself: its name, category, whether it is enabled, what hardware it claims, and
crucially **a list of actions with typed parameters**. See `api-samples/modules.json` — that is the
real 26 KB payload.

A single action looks like this:

```json
{
  "act": "type",
  "help": "type a string on the host computer",
  "params": [
    {"name":"text","type":"string","required":true,"help":"the text to type"},
    {"name":"wpm","type":"int","required":false,"help":"typing speed","min":1,"max":2000}
  ]
}
```

Parameter types are: `string`, `int` (with optional `min`/`max`), `bool`, `enum` (with a value
list), `enum_list` (multi-select).

**So you are designing a component system, not a set of screens.** New modules will be added later
(`msc`, `wifiscan`, `blescan`) and the UI must render them without a redesign. If your design
requires a bespoke screen per feature, it is the wrong design.

The interesting challenge: make something that is *generated* still feel considered rather than
like a database form. The most-used things should feel designed; the long tail should still work.

---

## 3. Hard constraints — these are not negotiable

| Constraint | Consequence |
|---|---|
| **No internet while connected** | No web fonts, no CDN, no icon libraries, no analytics, no external anything. Everything ships on the device. |
| **Content-Security-Policy: `default-src 'none'; connect-src 'self'`** | No external requests of any kind. Inline styles/scripts must be accounted for. |
| **Served from the device over plain HTTP** | Not a secure context. `crypto.subtle`, service workers and several other APIs are unavailable. |
| **Storage budget ~1–2 MB realistically** | 7.75 MB partition exists, but it also holds macros and logs. A framework is *permitted* if bundled and justified; a 500 KB one is not. |
| **Single user, single device** | No accounts, no multi-tenancy, no sync. |
| **Phone portrait is primary**, tablet secondary, desktop incidental | Design mobile-first and mean it. |

Assume a modern mobile Safari and Chrome. No IE, no legacy.

---

## 4. Authentication, and a genuinely unusual UX problem

Two factors:

1. Join the dongle's Wi-Fi network (WPA2).
2. Enter an **8-digit PIN**, which the device shows **on its own LCD** while unpaired. Exchange it
   for a session token.

The PIN on the little screen is the out-of-band channel — it is why the pairing means anything.
Once a session exists, the LCD stops showing it.

**Then it gets interesting.** There are three privilege levels:

- `AUTH_NONE` — not paired. Almost nothing is visible.
- `AUTH_TOKEN` — paired over Wi-Fi. Most things.
- `AUTH_PHYSICAL` — **only reachable by plugging a USB cable into a computer** and using a serial
  console. Reserved for: revealing the Wi-Fi password and PIN, formatting storage, rebooting, and
  choosing which firmware image boots.

So the UI will contain actions the user **cannot perform from the phone at all, ever** — not
"upgrade your plan", not "log in again", but "go and physically plug the device into a computer".
That is an unusual thing to communicate well. Getting it wrong means either a frustrating dead end
or a dangerous action becoming too easy.

The device tells you the level required per action, so you can render this state rather than
discovering it by failure.

---

## 5. Safety — this device types on a computer

The `hid` module makes the dongle act as a **keyboard attached to whatever computer it is plugged
into**. It can type anything.

Design implications you must take seriously:

- **Arming it should feel deliberate**, not like flipping a toggle in a list. When armed, it only
  takes effect after the device restarts, and the dongle's own LCD shows a warning badge.
- **Its live state should be obvious at a glance** from anywhere in the UI. "Is this thing
  currently a keyboard?" should never require navigation to answer.
- Firmware updates, storage formatting and deleting files are similarly consequential. Recursive
  delete exists and is bounded, but it is still a recursive delete.

Do not make destructive things pretty and inviting. Do not bury them either — burying breeds
workarounds. Aim for *unmistakable*.

---

## 6. What the device can actually do today

From `api-samples/modules.json`:

| Module | What it is | Notable actions |
|---|---|---|
| `hid` | USB keyboard | `type` (a string), `key` (one keystroke with modifiers), `release` (panic stop), `layout` |
| `storage` | microSD + internal flash, two volumes `/sd` and `/fs` | `list`, `read`, `write`, `delete`, `mkdir`, `verify`, `free`, `format` |
| `display` | The 160×80 LCD | `backlight`, `screen`, `refresh` |
| `http` | This Wi-Fi AP | `status`, `sessions`, `psk`, `pin` |
| `led` | A single RGB status LED | `set` (a colour), `auto` |
| `cdc` | The USB serial console | — (it is how the cable talks) |

Plus device-level operations: firmware upload, module enable/disable, reboot, storage browsing.

**Not built yet, but coming** — design so these slot in without a rework:
`msc` (expose the SD card to the host PC as a drive), `wifiscan` / `blescan` (radio survey tools),
and **macros** (stored, named, replayable key sequences — likely to become the marquee feature).

### Live events

A WebSocket pushes events without polling: heartbeats with free memory and uptime, progress on
long operations (file verification, firmware upload), and state changes. Long operations report
percentage progress. The UI should feel alive rather than requiring a pull-to-refresh.

---

## 7. States you must design for

Not just the happy path:

- **Not connected to the AP** (the page will not load at all — but consider what a stale tab shows)
- **Connected, not paired** — PIN entry
- **Paired**, normal operation
- **An action that needs `AUTH_PHYSICAL`** — visible, explained, not performable here
- **A module that is disabled** — off, but present
- **A module blocked by another** — e.g. two modules want the same hardware; the device reports
  exactly which module is blocking, by name
- **A module armed but pending a restart** (`hid`)
- **Long operation in progress** with percentage, and the UI otherwise still usable
- **Firmware upload** — a multi-second transfer during which the device is largely unresponsive
- **Errors** — every failure returns a stable machine-readable `code` and a written `msg`. The
  messages are deliberately specific and actionable; show them, do not paraphrase them
- **Empty** — no files on the card, no macros yet, no card inserted at all
- **The device rebooting** — the session survives or does not; the page must cope

---

## 8. Physical context

- Used **one-handed on a phone**, often standing at a bench.
- The dongle itself may be **behind a computer, out of sight** — so the phone is the primary
  display, and the LCD is a glanceable secondary.
- A workshop is **bright and cluttered**. Contrast matters. Small text does not survive.
- Dark mode is worth considering; the device is often used in a garage/workshop at night.

---

## 9. Tone

The firmware's own voice is direct, specific and honest. Its error messages read like:

> `module 'hid' is armed and will bind its USB interface at the next boot; it is NOT running now. Reboot to apply.`

> `refusing to delete the card root "/"; name a file or directory inside it`

Match that. Precise, calm, no exclamation marks, no cheerfulness, no apologising. Assume the reader
is competent and tell them exactly what is true.

---

## 10. What exists today

A deliberately minimal bring-up UI: a PIN box, one card per module, a text field per action, and a
log. It works and is ugly, and it was never meant to survive. Do not treat it as a starting point
or feel bound by it. It is described here only so you know what is being replaced.

Its one good idea worth keeping: it renders entirely from the device's descriptors, so it already
displays modules it knows nothing about.
