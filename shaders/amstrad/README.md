# Amstrad monitor shader presets

CRT presets for the three monitors that match the eras this core emulates:

| preset | monitor | ships with | tube |
|---|---|---|---|
| `amstrad-monitor-ctm644.glslp` | CTM644, colour | CPC 664 / 6128 | 3701B22-TC20, 37 cm / 14", slot mask, 24 kV |
| `amstrad-monitor-gt65.glslp` | GT65, green mono | CPC 664 / 6128 | Orion 310GNB31, 12", **phosphor P31** |
| `amstrad-monitor-cm14.glslp` | CM14, colour | 464+ / 6128+ / GX4000 | Orion 370KRB22-TC21, 14", Toshiba blackstripe |

Tube part numbers and the P31 designation come from the Amstrad service
manuals; stripe pitch, deflection angle and video bandwidth are **not** in
them, so anything geometric here is era-typical rather than measured. The
files say which is which, inline.

## Install

The presets use paths relative to their own location, and expect to sit
beside RetroArch's stock `crt/` directory:

    <shaders_glsl>/amstrad/amstrad-monitor-*.glslp
    <shaders_glsl>/amstrad/shaders/cpc-video-bandwidth.glsl
    <shaders_glsl>/crt/...                     (stock, provides crt-guest-dr-venom)

On a typical Linux install `<shaders_glsl>` is
`/usr/share/libretro/shaders/shaders_glsl`.

## Use

    retroarch --set-shader <path>/amstrad-monitor-ctm644.glslp -L <core> <content>

`video_shader_enable = "true"` must be set in the RetroArch config, or
`--set-shader` is silently ignored and the log says "Stock GLSL shaders will
be used" without ever naming the preset.

For `amstrad-monitor-gt65.glslp`, also set the core option
`sugarbox_monitor = green`. The core does the luma conversion and P31 tint
itself; the preset deliberately contains no tint pass, because
`color-mangler.glsl` and `afterglow.glsl` both declare a parameter named
`sat` and `.glslp` parameters are global across passes.

## The extra pass

`shaders/cpc-video-bandwidth.glsl` is ours (public domain) and runs before
the stock 11-pass crt-guest chain. It models the limited bandwidth of the
monitor's video amplifier, horizontally only.

It exists because crt-guest's `h_sharp` cannot do the job: its kernel is
`exp2(-h_sharp * w^2)`, so even at the `h_sharp` floor of 1.0 a tap three
texels out weighs about 0.002 -- an effective reach under ±1.5 texels. In
this core's native 800x280 output a Mode 1 pixel is 2 texels wide and a
**Mode 0 pixel is 4**, so `h_sharp` can never blend adjacent Mode 0 pixels.
On real hardware you can barely tell Mode 0 from Mode 1; without this pass
Mode 0 renders as clean fat blocks, which is the giveaway of an emulator.

Adjustable live: **F1 → Shaders → Parameters → "CPC video bandwidth (texels)"**,
range 0.0–4.0, 0 disables the pass. Default 1.10, about half a Mode 0 pixel.

## Note on CM14 vs CTM644

These two are optically the same preset. Both monitors are 14" stripe
phosphor tubes and nothing found in the service manuals or CRT databases
distinguishes them visually. The CM14 file is kept for era clarity, not
because the picture differs.
