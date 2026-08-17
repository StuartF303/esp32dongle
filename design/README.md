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

## Status of the data

Auth levels are being added per-action as this is written. `help.json` already shows the shape
(`min_auth`, `allowed`); `modules.json` will gain the same per action and per module shortly. Design
against that shape — assume every action carries a required level and every module a minimum level
to be visible at all.
