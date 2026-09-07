# SugarLibRetro — status

Source-verified feature/gap tracker for the libretro Amstrad CPC core built on
`Tom1975/CPCCore` (CPCCoreEmu, MIT). Companion to the full audit history in
memory (`project_sugarbox_libretro_core_matrix.md`) — this file is the living
summary, kept in the repo so it travels with the code.

Compared against: **libretro-cap32**, **libretro-crocods**, standalone
**Caprice32**, **ACE-DL**, **JavaCPC**, **Retro Virtual Machine**, **Arnold**,
**CPCEC**, **WinAPE**, **CPCemu**.

## Licensing rule (applies to every "Reference" below)
CPCCore/CPCCoreEmu is MIT (ours). Everything else surveyed is GPL-2/3 except
where noted, and **libretro-cap32's own `libretro/` subtree is explicitly
non-commercial + GPL-incompatible** — never copy from it. Rule for all
"Reference" entries below: **read for algorithm/behavior, reimplement
clean-room in our own MIT code. Never copy source.**

---

## Done (shipped, verified — not screenshots-only, real captures/RAM reads)

| Feature | Note |
|---|---|
| Flux disk (IPF/CAPS/CTR/RAW/HFE/SCP) | boots real games (Airwolf DSK, After Burner IPF, 1942 RAW, HFE demo). Unique among libretro cores |
| Tape load + autorun | works |
| Cart/CPC+/GX4000/ASIC | works |
| Live 5-model switch, no restart | unique — crocods dead code, cap32 restart-based |
| Autorun (catalogue-driven, `GetCat()`) | immune to cap32's English-language autorun bug (#73/#154) |
| AMSDOS ROM wired (slot 7) | fixed 2026-09-07 — disks never booted before this |
| Green/amber monochrome monitor | table stakes, now matched |
| Per-model CRTC type + override | only ACE/ACE-DL matches |
| Keyboard layout (uk/fr) | — |
| Floppy sound + real audio output | engine's `InitSound` overload was never called — was totally silent before this fix |
| Lightgun device wired | builds, no crash. Hit-detection UNVERIFIED (no test software found) |
| Disk-control EXT interface | real filenames, last-disk restore — matches cap32 |
| M3U multi-disk + drive B | 2 silent ordering bugs fixed |
| Disk write-back (EDSK) | sidecar default, overwrite opt-in, weak-sector guard |
| Emulated write-protect tab | — |
| Border crop (normal/full) | — |
| PlayCity (2nd AY + Z80 CTC) | wired via public `CSig::exp_list_`, default off. NOT unique — ACE-DL + JavaCPC have it too |
| RAM export + memory map | RetroArch cheat/debug support |
| Multiface II ROM stripped from binary | was shipping Romantic Robot's commercial firmware in our .so |
| Headless RAM-probe test harness | `tools/cpc_probe.sh` — permanent dev tooling |
| .info metadata, input descriptors, PAL region fix | — |

## WIP / partial

| Item | Status |
|---|---|
| Lightgun | wired, accuracy unverified — no CPC lightgun software found |
| No-env disk autorun landing | armed correctly per logs; "actually landed" proof still open (two RAM oracles tried, both non-discriminating) |
| M3U/drive-B | works, not stress-tested like cap32's chronic failure history |

---

## Missing — placement + priority

### Tier 1 — libretro.cpp only, no CPCCoreEmu touch needed (do these first)

Ranked by value:

1. **Printer-to-file.**
   `IPrinterPort` is dependency-injectable: `CSig::printer_port_` is a
   **public** member (`Sig.h`), same DI slot pattern already used for
   `RetroDisplay`/`RetroSound`/`RetroFdcNotify`. Engine's own
   `PrinterDefault` is dead (`if (false) // TODO` stub) but we don't need to
   fix it — just point `printer_port_` at our own `IPrinterPort`
   implementation and never touch the engine class. Genuinely new finding
   this pass; earlier notes wrongly filed this as engine-only.
   Reference (read-only): JavaCPC's `TextPrinter`→`FileWriter`, Arnold's
   printer-to-file. Output format is trivial (raw 7-bit text dump) — no real
   need to study anything, could write clean-room from the CPC printer
   protocol alone.

2. **Tape record/save.** Engine already exposes everything public via
   `EmulatorEngine::GetTape()`: `SaveAsWav`, `SaveAsCdtDrb`, `SaveAsCSW`,
   `SaveAsCdtCSW`, `Record()`, `Rewind()`, `IsTapeChanged()`. This was
   attempted once and reverted because a recorded CDT wouldn't reload after
   `Rewind()` — worth re-attacking, since the API is a save-as model (not
   write-back), so the bug is most likely in how libretro.cpp decides *which
   file* becomes "the tape" after saving, not in the engine. No GPL
   reference needed — pure wrapper debugging.

3. **On-screen/virtual keyboard.** Pad-only handheld targets need d-pad
   navigation (pointer-driven overlays are dead on arrival here), no
   font/glyph system exists in this core today (rectangles only) — real
   work either way, but 100% in libretro.cpp.
   Reference (read-only, `libretro/` subtree — do not copy): cap32's
   `retro_keyboard.c` + microui overlay, for layout/UX ideas only.

4. **Combo-keys** (joypad button → CPC key combo). Same mechanism already
   proven for autorun (`ForceKeyboardState`) — a core option table plus
   OR-ing extra matrix bits. libretro.cpp only.
   Reference (read-only): cap32's `combokey`/`db_mapkeys`.

5. **Lightgun hit-detection verification.** Not code — needs CPC
   lightgun-compatible test software, none found yet. Independent of both
   sides.

### Tier 2 — needs a CPCCoreEmu (submodule) change — raise with Thomas first

1. **Multiface II full wiring.** `multiface2_` sits under `protected:` in
   `Motherboard.h` with no accessor (unlike `play_city_`, which has a public
   `GetPlayCity()` — that asymmetry is exactly why PlayCity could be wired
   without Thomas and Multiface can't). Needs one accessor line
   (`GetMultiface2()`) *plus* real paging-order work: calling
   `MultifaceStop()` today fires an NMI with no MF2 ROM paged in.
   Reference (read-only): Caprice32 standalone — the only working
   implementation among everything surveyed.

2. **Plain DSK / RAW / CTRAW disk write.** All three `SaveDisk()` overrides
   return `NOT_IMPLEMENTED` in the engine. Needs a real write algorithm
   added to CPCCoreEmu (most `.dsk` in the wild are EDSK anyway, which
   already works — lower real-world urgency than it looks).
   Reference (read-only): CPCEC's `cpcec-d7.h` — full worked algorithm incl.
   in-place `MV → EXTENDED` conversion. GPL-3.0, reimplement clean-room only.

3. **AMX/Kempston mouse.** Confirmed zero mouse abstraction anywhere in
   CPCCoreEmu (grepped `IJoystick`/`IMouse`/`Kempston`/`AMX` — no hits). Not
   a wiring gap like PlayCity/Multiface, a genuine from-scratch port-decode
   class. No cleanly adaptable reference — ACE-DL/RVM are closed binaries,
   Arnold/JavaCPC are GPL. Lowest ROI of the engine-side items.

4. **Second lightgun port / dual-joystick Y-cable.** `CRTC` holds one
   global `gun_x_`/`gun_y_`/`gun_button_` — no per-player state, so a second
   gun needs an engine change, not just a second libretro device slot.

5. **`retro_serialize()` side-effect-free save.** No synchronous
   `SaveSnapshot` path in the engine; current implementation ticks the
   emulator (`RunUntilSnapshotWritten`), which violates the libretro
   contract.

6. **`Z84C30::In()` empty stub.** PlayCity's CTC always reads back the
   floating bus (255) — small, already flagged upstream-worthy alongside
   the above.

---

## Corrections made this pass
- **Digiblaster is NOT in CPCCore at all** (grepped, zero matches
  anywhere). An earlier note calling it an "unwired Tier-C peripheral" like
  Multiface/PlayCity was wrong — there is no code to enable, it would be
  from-scratch engine work same as AMX mouse.
- **Printer-to-file moved from "needs Thomas" to "libretro-side, doable
  now"** — found the public `CSig::printer_port_` DI slot.

---

## Nice-to-have / no clear placement yet
- Firmware-missing error UI (`.info` firmware declaration already shipped;
  no ROM-missing detection/UI exists in any surveyed codebase to model
  after).
- Overscan/border fine-tuning beyond normal/full (cap32's `scr_crop` is more
  granular).

---

*Kept in sync with `project_sugarbox_libretro_core_matrix.md` (full audit
trail, evidence, commit IDs). This file is the summary; that file is the
receipts.*
