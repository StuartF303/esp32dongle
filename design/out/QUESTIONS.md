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

