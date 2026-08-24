# What to hand back

The output of the design work goes back to an engineer (Claude Code) who will implement it as
static assets served from the device's LittleFS partition. Design for that reality.

Put everything under `design/out/`.

---

## Deliverables, in priority order

### 1. `out/DESIGN.md` — the reasoning (most important)

Not a style guide. The **decisions and why**, including:

- The information architecture: what the top level is, what nests, what the user sees first and why.
- How you resolved the central problem in §2 of the brief — making a *generated* UI feel designed.
  Which things get bespoke treatment, which fall back to the generic renderer, and where you drew
  that line.
- How `AUTH_PHYSICAL` actions are presented (§4). This is the most novel problem here.
- How `hid` being live is surfaced globally (§5).
- What you deliberately did **not** do, and why. This is as useful as what you did.

Be concrete and opinionated. "It depends" is not a deliverable.

### 2. `out/*.html` — working static prototypes

Real HTML/CSS, openable in a browser, not images. They do not need to be wired to a device — stub
the data from `api-samples/`. Priority screens:

- **pair** — PIN entry, unpaired state. Show all four of it: typed entry, arrival by QR scan
  with the PIN already supplied, reconnecting inside the grace window, and the lockout (§4.4)
- **home** — whatever you decide the landing view is
- **module detail** — the generic renderer, shown with `storage` (many actions, several parameter
  types) and with `hid` (dangerous, has a pending-restart state)
- **file browser** — two volumes, a real listing, empty and error states
- **firmware update** — file picker, progress, the reboot-required outcome
- **an `AUTH_PHYSICAL` refusal** — in context, not as an alert

Self-contained files. No CDN, no build step, no external fonts — see the constraints table.

### 3. `out/tokens.css` — the system

Custom properties for colour, type scale, spacing, radii, motion. Include a dark variant if you are
proposing one. This is what gets lifted directly into the implementation, so make it complete and
name things clearly.

### 4. `out/COMPONENTS.md` — the parts

For each reusable piece: what it is, its states (default / disabled / loading / error / forbidden),
and how the generic parameter renderer maps each type — `string`, `int` (with `min`/`max`), `bool`,
`enum`, `enum_list` — to a control.

---

## Rules

- **Everything ships on the device.** No external requests of any kind, at any point, including
  fonts. If your design needs an icon set, it must be inline SVG you provide.
- **Mobile portrait first.** Test at 390×844. Tablet is secondary; desktop can be a widened
  version of the same thing.
- **Use the real data.** `api-samples/` is captured from the running device — real module names,
  real action lists, real error message wording. Do not invent friendlier copy for the errors; the
  firmware's messages are deliberate and will be shown verbatim.
- **Accessibility is not optional**: real contrast, touch targets ≥44 px, visible focus, semantic
  markup, and a working keyboard path. The workshop context makes contrast a functional
  requirement, not a checkbox.
- **Size matters.** Roughly 1–2 MB total. A framework is allowed if bundled and justified in
  `DESIGN.md`; vanilla is entirely acceptable and probably wise.

---

## Questions

If something is genuinely ambiguous, write the question into `out/QUESTIONS.md` and **proceed with
a stated assumption** rather than blocking. Note both. The engineer will resolve them and can
iterate.

Where you think the brief is wrong — including the choice to keep it descriptor-driven — say so.
That is a legitimate finding, not a deviation.
