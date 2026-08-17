# How we work on this together

Shared reference for everyone working on `esp32dongle` — Stuart, the design side, and the
firmware side. Read `README.md` first for what the thing actually is.

---

## Who does what

**Stuart** owns the device and every decision that is irreversible, security-relevant or
architectural. He is technical and reads the code. He is asked, not informed, about: anything
that changes what boots, anything that changes the security posture, and anything expensive to
undo. Routine calls get made and reported, not queued up for him.

**Design** owns the interface: information architecture, interaction, visual system, copy for
anything that is not a device error message. Works in `design/`, delivers into `design/out/`.
Does not edit `firmware/`.

**Firmware** owns everything under `firmware/`, the protocol, and the implementation of designs.
Delivers the UI as static assets served from the device. Does not redesign in the process of
implementing — where a design cannot be built as drawn, that comes back as a question.

---

## Where things live

| Path | Owner | |
|---|---|---|
| `README.md` | shared | What the project is. Entry point. |
| `docs/ARCHITECTURE.md` | firmware | Design, decisions, and the reasoning. The record of *why*. |
| `docs/BACKLOG.md` | shared | Deferred work **with the reasoning that deferred it**. Not a to-do list. |
| `design/BRIEF.md` | firmware → design | The brief. |
| `design/HANDBACK.md` | firmware → design | What design delivers and in what form. |
| `design/api-samples/` | firmware | **Real payloads captured from the device.** Not mock-ups. |
| `design/out/` | design | Design output. Implemented from, not edited by firmware. |
| `firmware/` | firmware | The ESP32-S3 code. |
| `CLAUDE.md` | firmware | Verified hardware facts and hard-won gotchas. |

---

## The loop

1. Firmware captures real device data into `design/api-samples/` and writes the brief.
2. Design works in `design/out/` — reasoning in `DESIGN.md`, working HTML prototypes, tokens,
   component specs.
3. Firmware implements from `design/out/`, and reports back anything that could not be built as
   drawn **and why**.
4. Stuart arbitrates anything the two sides disagree about.

Questions do not block. If something is ambiguous, write it into `design/out/QUESTIONS.md`,
**state an assumption, and carry on**. Note both. A stated assumption that turns out wrong costs
one iteration; a blocked loop costs a day.

---

## Common understanding — the things that trip people up

**The UI is generated from device descriptors, not hardcoded per feature.** Modules describe
themselves and their actions with typed parameters. New modules get added and must render with no
front-end change. This is a component system keyed to parameter *types*, not a set of screens. If
a design needs a bespoke screen per feature, it will not survive the next module.

**There is no internet.** The phone is joined to the dongle's own access point, which routes
nowhere. No CDN, no web fonts, no icon libraries, no analytics. Everything ships on the device.

**Three privilege levels, and one of them means "go and plug it in".** `AUTH_PHYSICAL` actions
cannot be performed from a phone at all, ever — they need a USB cable and a serial console. The
UI has to show these as present-but-unreachable without being a dead end and without making
dangerous things feel casual.

**This device types on a computer.** The `hid` module is a real keyboard. Arming it should feel
deliberate, its live state should be obvious from anywhere, and destructive actions should be
unmistakable rather than either pretty or buried.

**Device error messages are shown verbatim.** They are deliberately specific and actionable
(`refusing to delete the card root "/"; name a file or directory inside it`). Do not paraphrase
them into friendlier copy — the specificity is the value.

**Size is a real budget.** UI assets live in a 7.75 MB partition shared with macros and logs, and
are transferred over the device's own AP. A framework is allowed if bundled and justified. A
500 KB one is not.

---

## Working conventions

- **Verify on hardware, not just in the build.** This project has twice shipped code that built
  clean, passed every host test, and could never run. Build-clean is not evidence.
- **Record the reasoning, not just the change.** `docs/BACKLOG.md` and commit messages carry *why*
  something was deferred or decided. That is the part that is expensive to reconstruct.
- **Say what is unproven.** Anything not exercised on the device gets labelled as such.
- **Pin versions.** No floating dependencies anywhere.
