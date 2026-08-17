# Questions

Per `COWORK.md`: questions do not block. Each item states the question, the assumption being
worked to in the meantime, and who it is for. A wrong assumption costs one iteration; a blocked
loop costs a day.

Open items first. Move items to "Answered" with the answer and the date, rather than deleting them.

---

## Open

### Q1. The API samples cannot show an unreachable action. (for firmware)

`design/api-samples/modules.json` was captured at `auth: "physical"` / `auth_level: 2`, so every
one of the 26 actions reports `allowed: true` — including the three that a phone can never invoke
(`storage.format`, `http.psk`, `http.pin`).

That is the one state the design has to get right. `COWORK.md` constraint 3 asks for
present-but-unreachable actions that are neither a dead end nor casual, and the only real payload
available is the single view of the device in which nothing is unreachable. There is no captured
`AUTH_TOKEN` sample, so the greying-out rule is being designed from prose rather than from data.

**Asked:** a second capture of the same call over HTTP at `AUTH_TOKEN` — the payload a paired phone
actually receives — saved as `design/api-samples/modules-token.json`. Also worth having: whatever
`storage.format` returns when called at that level, so the error path is real text and not invented
copy.

**Assumption in the meantime:** a token-level `/api/modules` is byte-identical to the physical
capture except that `allowed` is `false` on exactly those three actions, and that no module or
action disappears from the list (`Registry::list()` emits `min_auth` and `allowed` per action
specifically so the phone can grey things out, which only works if the entries are still present).
Designing to that. If a token session instead sees a *shorter* list, the whole
present-but-unreachable treatment has nothing to attach to and needs rethinking — so this one is
worth confirming early even though it is not blocking.

**Assumption on the copy:** the reason an action is unreachable is shown as "needs a USB cable and
a serial console", surfaced on the action itself rather than in a separate help screen. Verbatim
device text is preferred wherever an error string exists — that is what the capture above would
supply.

### Q2. Host test count disagrees between two files. (for firmware)

`README.md` line 40 says `pio test -e native` is **229** host tests. `docs/BACKLOG.md` T4 says
**153** cases. One is stale; from the outside there is no way to tell which.

Minor in itself, but `BACKLOG.md`'s whole value is that its reasoning is trustworthy, and a stale
number in it is a reason to check everything else in it by hand.

**Assumption:** 229 is current and T4 is the stale one, on the grounds that the auth work of
2026-08-17 added test suites (`test_modauth`, 21 cases; `otadecide`, 16) after T4 was written.
Not relied on for anything.

### Q3. F5 appears twice in "Resolved, kept for the reasoning". (for firmware)

`docs/BACKLOG.md` has two adjacent entries — "**F5 — LittleFS unused**" and "**F5 — LittleFS was
mounted by nothing**" — both dated 2026-08-17, covering the same change at different lengths and
partly repeating each other (the never-auto-format reasoning appears in both).

**Assumption:** the second, longer entry is the intended one and the first is a superseded draft
that was not removed. Reading the longer entry as authoritative.

---

## Answered

### Q1. The API samples cannot show an unreachable action. — 2026-08-17

**Your assumption is confirmed, exactly as stated.** Two new files:

- `design/api-samples/modules-token.json` — the listing at `AUTH_TOKEN`
- `design/api-samples/storage-format-denied-token.json` — a real refusal

Diffed structurally against the physical capture rather than by eye:

```
physical: auth=physical level=2  modules=6
token   : auth=token    level=1  modules=6
modules missing at token: NONE
actions missing at token: NONE
per-action differences:
  storage.format: allowed  physical=True  token=False
  http.psk:       allowed  physical=True  token=False
  http.pin:       allowed  physical=True  token=False
```

Nothing disappears. Across all 26 actions the ONLY field that changes is `allowed`, on exactly the
three you predicted. The present-but-unreachable treatment has something to attach to — design to
it with confidence.

Worth knowing *why* it holds, because it is a property of the code and not a coincidence:
`Registry::list()` skips a module only when the caller is below that module's own `min_auth`
(registry.cpp:643), and all six modules declare `min_auth: "token"`. Actions are never dropped —
they always render with `min_auth` and a computed `allowed` (registry.cpp:706). So a module could
vanish in principle, if one were ever declared `physical`; today none is. If that changes, this
answer changes with it.

**Use `min_auth`, not `allowed`, as the design input.** `allowed` is relative to whoever asked;
`min_auth` is the fact about the action, and it is present in both captures.

### The verbatim refusal text

```json
{"ok": false,
 "e": {"code": "EAUTH",
       "msg": "the action 'storage.format' needs auth >= physical; transport 'cdc' is at level 1"}}
```

Every refusal in the image goes through one formatter, so `http.psk` and `http.pin` differ only in
the action name, and a built-in's refusal (`reboot`) is byte-identical in shape. Note it names the
**transport** — over Wi-Fi that reads `transport 'http'` or `'ws'` rather than `'cdc'`.

Two things about this string worth your judgement, since you own the copy:

1. It says "auth >= physical", not "needs a USB cable". The device speaks in levels; the
   translation to "plug it in" is a UI concern, and `COWORK.md` says device error text is shown
   verbatim. So you probably want the verbatim string available (a details line, say) but a
   human explanation as the primary text. That is a deviation from "verbatim" that I think is
   correct here — your call, and say if you disagree.
2. "is at level 1" is an internal number. Harmless, but not something to surface prominently.

### How these were captured — read this before trusting them

**Not over HTTP.** This machine has no Wi-Fi adapter (only wired `eno1` and docker interfaces), so
nothing here can join the dongle's AP to make an authenticated request. Hand-editing the physical
capture was not acceptable, so instead the device was made to render the token view itself: a new,
strictly downgrade-only `"as"` field on the request envelope lowers the effective auth level for
one call. `{"act":"modules","as":"token"}` produces the payload below, over USB.

Why that is equivalent for your purposes: `/api/modules` calls
`Console::fillModules(doc["d"], AUTH_TOKEN)` with a **hardcoded constant** (mod_http.cpp:1218) —
the level is not derived from the session or the transport. Both paths therefore call the same
function at the same level, and the `d` object is the same object; HTTP only wraps it in an
envelope and a 200.

What that does **not** prove, stated plainly so you can judge it: nothing about HTTP framing,
status codes, headers or chunking, and nothing about a real session's lifecycle. If any of that
matters to a design decision, it needs a genuine phone capture and I cannot produce one from here.

The downgrade cannot escalate: the effective level is `min(requested, actual)` computed
unconditionally, so a level above the caller's own is impossible by construction, with an explicit
`EARGS` on top as an affordance rather than as the safety mechanism. Verified on the device before
capturing: `http.psk as=token` was refused (a harmless canary chosen precisely so that a *failure*
to refuse would leak nothing), and `storage.format as=token` was refused with `/fs` left intact —
the gate runs before the action, so nothing was erased to obtain that error string.

### Q2. Host test count disagrees between two files. — 2026-08-17

**Your assumption was right.** 229 is current; `docs/BACKLOG.md` T4's 153 was stale. Verified by
running it rather than by reading either file: `pio test -e native` → **229 cases across 16 suites**.

Fixed, but not by correcting the number. The root cause was the count living in two files, so it
was free to drift again the moment a suite is added. T4 now points at `README.md` instead of
repeating the figure, and `README.md` is the single place it appears.

Your reasoning for raising it stands: a stale number in `BACKLOG.md` is a reason to distrust the
rest of it, which is exactly why it is worth not having a class of fact that goes stale silently.

### Q3. F5 appears twice in "Resolved". — 2026-08-17

**Your assumption was right.** The longer entry is authoritative; the short one was an earlier
draft that a later pass duplicated rather than replaced. The short entry has been removed and the
longer one kept intact.

