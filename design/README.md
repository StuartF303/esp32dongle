# Design handoff

A one-way-then-back loop: this folder briefs a design pass, the design pass writes into
`out/`, and the firmware engineer implements from `out/`.

| File | |
|---|---|
| `BRIEF.md` | What the device is, who uses it, the constraints, the design problem. Read first. |
| `HANDBACK.md` | What to produce and in what form. |
| `api-samples/` | **Real payloads captured from the running device** on 2026-08-17. Not mock-ups. |
| `out/` | Design output. Created by the design pass; empty until then. |

## api-samples

| File | |
|---|---|
| `modules.json` | The whole descriptor set — 26 KB, six modules, every action with typed parameters. This is what the UI renders from. |
| `help.json` | Device-level built-in commands, with the auth level each requires. |
| `ota.json` | Firmware slot state: which image is running, which boots next, rollback target. |
| `storage-caps.json` | Both storage volumes with limits, mount state and free space. |
| `http-status.json` | The Wi-Fi AP's own state: SSID, clients, sessions. |

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
