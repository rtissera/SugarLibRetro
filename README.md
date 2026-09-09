# SugarLibRetro

Master branch build status : 
[![Build Status](https://travis-ci.com/Tom1975/SugarLibRetro.svg?branch=master)](https://travis-ci.com/Tom1975/SugarLibRetro)
[![Build status](https://ci.appveyor.com/api/projects/status/jfdxa7debkgm40nx/branch/master?svg=true)](https://ci.appveyor.com/project/Tom1975/sugarlibretro/branch/master)

Retroarch Core emulating Amstrad CPC/CPC+/GX4000

See [`SugarLibRetro/STATUS.md`](SugarLibRetro/STATUS.md) for the current
feature matrix and gap-priority tracker.

## Firmware

The CPC firmware ROMs and the Plus/GX4000 system cartridge are compiled into
the core, so it runs with an empty system directory. Amstrad have kindly given their permission for the
redistribution of their copyrighted material but retain that copyright. The
embedded dumps are unmodified.

To use your own dumps instead, put them in
`<system directory>/amstradcpc/ROM/` under the names listed in
`SugarLibRetro/tools/embed_roms.py`; a file on disk always takes precedence
over the embedded copy. Run that script to regenerate `embedded_roms.h` after
changing the bundled set.

## Testing

`SugarLibRetro/tools/cpc_probe.sh` types BASIC into a running core and reads
the result back out of RAM over RetroArch's network command interface, so
core changes can be verified with numbers instead of screenshots. See the
script's header comment for usage.

`SugarLibRetro/tools/test_corpus.sh` runs a format-parser regression pass: it
loads one real sample of every disk/tape format the core supports (DSK,
EDSK-with-weak-sectors, HFE, HFEv3, SCP, IPF, CTR/RAW, CDT) and checks the
core survives loading each one headlessly. The corpus itself is **not** in
this repo — several samples are copyrighted commercial game/software dumps,
kept locally only, needed because copy-protection-relevant formats (EDSK weak
sectors, IPF flux protection) essentially don't exist in freeware form. Get it
by asking whoever maintains it for a copy, or rebuild it from
`Tom1975/CPCCore`'s own `UnitTests/res/` fixtures (see that project's test
suite) plus one freeware IPF (e.g. Orion Prime). Point the script at your copy
via `CPC_CORPUS_DIR` (default `~/cpc-test-corpus`).

