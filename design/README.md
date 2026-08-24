# Design handoff

A one-way-then-back loop: this folder briefs a design pass, the design pass writes into
`out/`, and the firmware engineer implements from `out/`.

| File | |
|---|---|
| `BRIEF.md` | What the device is, who uses it, the constraints, the design problem. Read first. |
| `HANDBACK.md` | What to produce and in what form. |
| `api-samples/` | **Real payloads captured from the running device**, 2026-08-17 and re-captured 2026-08-24. Not mock-ups. |
| `out/` | Design output. Created by the design pass; empty until then. |

## api-samples

| File | |
|---|---|
| `modules.json` | The whole descriptor set — 26 KB, six modules, every action with typed parameters. This is what the UI renders from. |
| `help.json` | Device-level built-in commands, with the auth level each requires. |
| `ota.json` | Firmware slot state: which image is running, which boots next, rollback target. |
| `storage-caps.json` | Both storage volumes with limits, mount state and free space. |
| `http-status.json` | The Wi-Fi AP's own state: SSID, clients, sessions, the grace window, the DNS responder. |

### Re-captured 2026-08-24, and what moved

`http-status.json`, `modules.json`, `modules-token.json` and `ota.json` were re-taken from the
device after the single-client pairing change and the captive-portal work. `help.json` and
`storage-caps.json` came back **byte-identical** to the 2026-08-17 capture, so they were already
current rather than stale.

What changed that the design side needs to know about:

- **`max_clients` and `max_sessions` are both `1`**, not 4. They were 4 in the first capture and
  that is the single most misleading thing in the old set — see `BRIEF.md` §4.
- **`station_associated`** is new and is the field `BRIEF.md` §4.2's "live vs reconnecting"
  distinction actually turns on. With a single-client AP it is the session's lifeline, not a count.
- **`grace_ms`** (always present, 90000) and **`grace_active`** / **`grace_ms_left`** (only while a
  window is armed) are the reconnecting state made readable.
- **`dns_up` / `dns_heap_bytes`** report the captive-probe DNS responder. The `424` in this capture
  is the *second* enable of that boot: most of the cost is a shared framework task created once and
  never freed, so the recurring figure is small and the one-off is not.

  **`dns_heap_bytes` is a jittery delta, not a fixed figure — do not put the number in a UI as if
  it were one.** It is two `ESP.getFreeHeap()` samples with a construct-and-start between them, so
  it picks up whatever else the allocator did in that window. Re-measured 2026-08-24: `5168` on
  three consecutive cold boots (and `5388` once in an earlier session), then `204` **or** `424` on
  later enables, alternating unpredictably across 48 disable/enable cycles. The *shape* — about
  5 KB once, then a couple of hundred bytes — reproduces every time; the exact values do not.
- **`modules-token.json` vs `modules.json` is still the pair to diff**, and it always was. The
  2026-08-17 files were the same *size* — 28,804 bytes each, which is a coincidence: `physical` →
  `token` loses three characters, `2` → `1` loses none, and `true` → `false` on three actions gains
  three. They were never byte-identical, and the Q1 answer in `out/QUESTIONS.md` describing their
  differences was accurate. `auth` reads `token`, `auth_level` is `1`, and the three
  `AUTH_PHYSICAL` actions come back `allowed: false` while still being listed. That is §4 of the
  brief made concrete.

**`storage-format-denied-token.json` was deliberately NOT re-captured.** Producing it means
sending `storage.format` at the token level and relying on the refusal; nothing in the auth
declarations for that action moved in this pass, so the recorded refusal is still exact, and
re-running a format command against a card with data on it to confirm a message that has not
changed is not a trade worth making.

Captured over the USB serial console, which runs at the highest privilege level — so these show
**everything**, including actions a phone session cannot perform. That is deliberate: the design
needs to know what exists in order to show it as unavailable.

## Auth levels are in the data

Every module carries `min_auth` (the level needed to know it exists) and every action carries both
`min_auth` (absolute) and `allowed` (relative to whoever asked). A caller below a module's level
does not see it in the listing at all.

These captures were taken over USB, i.e. at `physical`, so every `allowed` reads `true`. **Use
`min_auth`, not `allowed`, to design the states** — `allowed` is whatever the current session can
do, `min_auth` is the fact about the action.

Three actions cannot be performed from a phone at all, ever. They are §4 of the brief made
concrete, and the design needs to show them as present-but-unreachable rather than hide them:

| action | `min_auth` | why |
|---|---|---|
| `storage.format` | `physical` | erases the whole internal volume |
| `http.psk` | `physical` | reveals/changes the Wi-Fi passphrase |
| `http.pin` | `physical` | reveals/regenerates the pairing PIN |

Plus, at device level (see `help.json`): `reboot` is `physical`, as are the OTA `confirm` /
`rollback` / `boot` parameters. Everything else is `token`.
