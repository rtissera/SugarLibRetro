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
| Format-parser regression corpus | `tools/test_corpus.sh` — real DSK/EDSK/HFE/HFEv3/SCP/IPF/CTR/CDT samples, checked headlessly |
| Printer-to-file capture | `sugarbox_printer_capture` (default off) — via the public `CSig::printer_port_` DI slot, no engine patch |
| Combo-keys | `sugarbox_combo_l/r/l2/r2` — shoulder buttons reach Enter/Space/Esc/Delete/Tab/Copy/Control/Capslock on a gamepad-only session |

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

1. ~~**Printer-to-file.**~~ **DONE (`bded622`)** — `sugarbox_printer_capture`,
   default off. Verified end-to-end headless: `PRINT #8,"..."` produced a
   byte-exact captured file when enabled, nothing when disabled.

2. ~~**Tape record/save.**~~ **RE-ATTEMPTED AND REVERTED AGAIN (2026-09-08)
   — moved to Tier 2, this is a real engine bug, not a wrapper bug.**
   Wired `sugarbox_tape_record` (experimental): arm `InsertBlankTape()` +
   `Rewind()` + `Record()` at load, `SaveAsCdtCSW()` at unload. Root-caused
   both problems from the original note with a real crash, not a guess:
   - **Segfault inside `CTape::Tick()`**, confirmed via `coredumpctl`/gdb
     backtrace: `retro_run → EmulatorEngine::RunFullSpeed →
     Motherboard::StartOptimizedPlus<...> → CTape::Tick()`, SIGSEGV. The
     faulting instruction indexes `tape_array_` with a garbage offset
     (register held `0x2fffffffd`, i.e. an unsigned value 3 short of a
     32-bit wraparound, times a small multiplier — a classic unsigned
     underflow). The recording path's own array-growth code does
     `sizeof(FluxInversion) * (nb_inversions_ - (tape_position_ + 1))`
     unsigned arithmetic — on a genuinely **blank** tape (`nb_inversions_`
     starts at 0), the first inversion trivially makes
     `tape_position_ + 1 > nb_inversions_`, and that subtraction wraps.
     **Recording onto an already-populated tape (real content, not
     `InsertBlankTape()`) did not crash** in the same test — strong
     supporting evidence for exactly this hypothesis, not proof by itself.
   - **Separately, even without the crash, the arm/disarm design is wrong.**
     Arming `Record()` for the whole session (there is no public
     `StopRecord()` — real hardware requires physically stopping the deck)
     captured **1.5 million tape inversions in 102 seconds** while replaying
     a real game's own tape-motor activity, nowhere near our actual typed
     `SAVE` command finishing. `record_`, once engaged, forces
     `CTape::Tick()`'s next-event interval to a fixed 4 T-states — i.e. it
     runs full speed for as long as the motor is on, recording whatever
     line level it sees, whether or not that's a real guest `SAVE` in
     progress. A usable wrapper would need a much narrower, motor-transition
     -driven arm/disarm window, which the current public API gives no clean
     hook for.
   Both problems point at CPCCoreEmu, not libretro.cpp: the crash is a real
   bug in the engine's own (apparently never-before-exercised) recording
   code, and the missing stop-recording primitive is a real API gap.
   **Raise both with Thomas** — do not re-attempt from the libretro.cpp side
   until the engine has a safe record-onto-blank-tape path and a way to end
   a recording deliberately.

3. ~~**On-screen/virtual keyboard**~~ **DONE, both options (`b126952`,
   `db5ef3a`, `69dc17f`, `0288d47`)** — Romain's call was both, not
   either/or. START opens/closes; Y switches between the two panels; in
   the grid, X is a sticky shift toggle (matches VICE from the survey)
   that produces a real Shift+letter — something the curated-command
   path's ArmAutorun/kAutorunKeys can't do, since it hardcodes every
   letter unshifted. First feature in this core needing real pixel
   verification, which caught two real bugs:
   - A throwaway "TEST" render caught a genuine environment gotcha before
     it could taint any real result: RetroArch's window can render larger
     than the Xvfb screen and get silently clipped by `xwd -root` (see the
     font comment in libretro.cpp) — needs Xvfb ≥1600x1200.
   - **The grid was originally one row per matrix scan line — electrical
     wiring order, not what's printed on a real keyboard.** Romain caught
     it. Line 2 alone holds `[` (sits next to P), `]` (sits next to L) and
     Return (spans both), three different visual rows sharing one
     electrical line. Rebuilt from a real CPC 6128 keyboard photo
     (cpcwiki.eu, via web.archive.org) and cross-checked against MAME's
     `amstrad.cpp` driver (came back byte-identical to the existing
     matrix data — only the grid's row *grouping* was wrong, not the
     underlying char/line/bit table). Now 5 real rows: Esc/digits,
     QWERTY, ASDF, ZXCV, Ctrl/Copy/Space.
   No panel on GX4000 (`0288d47`) — it's a console, no physical keyboard
   exists to have an on-screen one of.
   Full writeup incl. font glyphs, grid table derivation, the keyboard
   photo, and the state-machine design in `project_sugarbox_osk_survey.md`
   and the commit messages.

3a. **French ROM support, `sugarbox_rom_language` (`535c98d`) — new, but
    shipped with a real regression found and fixed the same round
    (`d7b824e`).** Prerequisite for a real (non-cosmetic) AZERTY OSK: the
    core only ever emulated a UK machine before this. Sourced from
    SugarboxV2's own ROM folder (same author as CPCCoreEmu; its UK files
    are md5-identical to the ones already bundled here, so the French pair
    next to them carries the same provenance/redistribution basis already
    established for UK). 464/6128 only — no French 664 ever existed.
    **Confirmed empirically that French firmware does a real keyboard
    remap, not a keycap relabel**: typed the identical matrix position
    (8,5) under UK and French ROMs — UK echoes `a`, French echoes `q`.
    This broke autorun immediately (`RUN"DISC"` typed via the UK-position
    `kAutorunKeys` table landed on screen as `run2disc` → Syntax error) —
    fixed by having `ArmAutorun` and the OSK refuse to type/open at all
    when `sugarbox_rom_language != "uk"`, logging why, rather than type
    wrong characters silently. **A real character-accurate French
    typing table and an actually-correct AZERTY OSK grid are still open**
    — this round only makes the emulated machine itself real; the
    automation built on top of UK positions stays UK-only until that
    table exists. Model-variant question resolved too: 464/664/6128/Plus
    share the same UK keyboard shape (no OSK changes needed); GX4000 has
    no keyboard at all (handled above).

4. ~~**Combo-keys**~~ **DONE (`96d5319`)** — `sugarbox_combo_l/r/l2/r2`,
   each mapping one shoulder button to Enter/Space/Esc/Delete/Tab/Copy/
   Control/Capslock/none. Verified end-to-end headless: an unsubmitted
   BASIC line (typed with no trailing Enter) only executed once the bound
   button was actually pressed via RetroArch's own input path.

5. **Lightgun hit-detection verification.** Not code — needs CPC
   lightgun-compatible test software, none found yet. Independent of both
   sides.

6. ~~**Real French keyboard typing**~~ **DONE (`50b62e1`)** —
   `kAutorunKeysFR` + `kOskGridFR`, built from real measured data (typed
   known UK positions under French firmware, read the actual echo back
   across four probe rounds — see `project_sugarbox_osk_survey.md` for the
   full transcript), not guessed by symmetry. Confirmed real classic
   AZERTY shape: only A/Q, W/Z swap and M relocates to UK's `:` position;
   digit row inverts UK's convention (unshifted → symbol/accented char,
   Shift → the digit). `RUN"DISC"` now boots a real game under French ROM
   (was `run2disc` → Syntax error before this table existed). Deliberate
   gaps, not guessed: `|` (so `|TAPE`/`|CPM`/`|A`/`|B` specifically don't
   work under French — `CAT`/`RUN"`/`NEW`/`LIST`/`CLS`/`MODE 0-2` do), a
   few untested punctuation positions, and every accented letter
   (é/è/à/ç/ù — not representable in `kOskFont`'s ASCII glyph set at all,
   a separate follow-up if ever needed).

### Tier 2 — needs a CPCCoreEmu (submodule) change — raise with Thomas first

1. **Tape record/save.** Real crash in `CTape::Tick()` (unsigned underflow
   recording onto a blank tape) plus a missing stop-recording primitive —
   see the retracted Tier-1 entry above for the full root-cause writeup and
   stack trace. Needs an engine fix before any wrapper attempt makes sense.

2. **Multiface II full wiring.** `multiface2_` sits under `protected:` in
   `Motherboard.h` with no accessor (unlike `play_city_`, which has a public
   `GetPlayCity()` — that asymmetry is exactly why PlayCity could be wired
   without Thomas and Multiface can't). Needs one accessor line
   (`GetMultiface2()`) *plus* real paging-order work: calling
   `MultifaceStop()` today fires an NMI with no MF2 ROM paged in.
   Reference (read-only): Caprice32 standalone — the only working
   implementation among everything surveyed.

3. **Plain DSK / RAW / CTRAW disk write.** All three `SaveDisk()` overrides
   return `NOT_IMPLEMENTED` in the engine. Needs a real write algorithm
   added to CPCCoreEmu (most `.dsk` in the wild are EDSK anyway, which
   already works — lower real-world urgency than it looks).
   Reference (read-only): CPCEC's `cpcec-d7.h` — full worked algorithm incl.
   in-place `MV → EXTENDED` conversion. GPL-3.0, reimplement clean-room only.

4. **AMX/Kempston mouse.** Confirmed zero mouse abstraction anywhere in
   CPCCoreEmu (grepped `IJoystick`/`IMouse`/`Kempston`/`AMX` — no hits). Not
   a wiring gap like PlayCity/Multiface, a genuine from-scratch port-decode
   class. No cleanly adaptable reference — ACE-DL/RVM are closed binaries,
   Arnold/JavaCPC are GPL. Lowest ROI of the engine-side items.

5. **Second lightgun port / dual-joystick Y-cable.** `CRTC` holds one
   global `gun_x_`/`gun_y_`/`gun_button_` — no per-player state, so a second
   gun needs an engine change, not just a second libretro device slot.

6. **`retro_serialize()` side-effect-free save.** No synchronous
   `SaveSnapshot` path in the engine; current implementation ticks the
   emulator (`RunUntilSnapshotWritten`), which violates the libretro
   contract.

7. **`Z84C30::In()` empty stub.** PlayCity's CTC always reads back the
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
