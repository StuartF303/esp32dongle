# qrcodegen — vendored

Nayuki's QR Code generator library, C port. Vendored rather than pulled from the PlatformIO
registry, for the reason `docs/BACKLOG.md` T2 gives: this project pins everything, and a
`lib_deps` entry that resolves at build time is not a pin.

| | |
|---|---|
| Upstream | <https://github.com/nayuki/QR-Code-generator> |
| Pinned commit | `8329a7108fc22be3e1eec0a9f9318978579e3621` (2024-09-01) |
| Files taken | `c/qrcodegen.h`, `c/qrcodegen.c` — unmodified, byte for byte |
| Licence | MIT, in the header of both files. Not covered by this repo's own `LICENSE`. |

## Why this one

Pure C99, no allocation, no dependencies — the caller supplies every buffer. That matters twice
over here: it compiles unchanged into `pio test -e native` alongside the firmware, so the
encoder is exercised on this machine, and it costs no heap on a board with 512 KB of SRAM and
no PSRAM.

Buffer sizes come from `qrcodegen_BUFFER_LEN_FOR_VERSION(n)`. At version 3 that is 107 bytes,
and two are needed (scratch + output) — about 214 bytes of stack for the largest code this
firmware renders. See `docs/ARCHITECTURE.md` §"QR pairing on the LCD" for which versions are
actually reachable and why the panel's 80-pixel height is the binding constraint.

## Updating it

Re-fetch both files at a new commit, update the SHA above, and re-run `pio test -e native`.
Do not edit the vendored sources: any local change belongs in the calling code, so that this
stays a straight copy that can be diffed against upstream.
