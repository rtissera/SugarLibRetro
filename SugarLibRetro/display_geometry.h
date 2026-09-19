#pragma once

#include <stdint.h>

// Display geometry and the coordinate transforms that depend on it.
//
// Header-only and free of libretro state on purpose: everything here is pure
// arithmetic, so it can be unit tested without standing up a core. libretro.cpp
// is one large translation unit of static functions and frontend callbacks,
// which is not testable; this is the part that has actual invariants.

// Visible window cut out of the emulator's internal raster buffer, which is
// 1024 ints wide with rows written at 2y (see RetroDisplay::GetVideoBuffer),
// so roughly 1008 x 576 of it is real picture.
//
// "normal" is the long-standing crop: picture plus a thin border, which is
// what most software expects. "full" widens it to show the CPC's overscan
// region, which demos and a fair amount of French software draw into. The
// frontend is told the full size as its maximum so the geometry can change at
// runtime without a reinit; cap32 exposes the same idea as cap32_scr_crop.
#define WIDTH  640
#define HEIGHT 480
#define OFFSET_X 207
#define OFFSET_Y 84

#define FULL_WIDTH  800
#define FULL_HEIGHT 560
#define FULL_OFFSET_X 112
#define FULL_OFFSET_Y 8

// The emulator writes one CPC scanline per two buffer rows, so a crop that is
// crop_h buffer rows tall is only half that many real lines. That halved
// figure is what the frontend is told and what is handed to video_cb.
static inline int SugarboxOutputHeight(int crop_h)
{
   return crop_h / 2;
}

// Lightgun. RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X/Y arrive as a signed 16-bit
// sweep of the visible picture; the CPC's CRTC wants a position in the raw
// buffer coordinate space, offset by the crop origin.
static inline int SugarboxGunBufferX(int16_t gun_x, int crop_w, int crop_x)
{
   return ((int)gun_x + 0x8000) * crop_w / 0x10000 + crop_x;
}

// Y is the subtle one. CPCCore tests
//    ((gate_array_)->monitor_)->y_ * 2 == gun_y_          (CRTC.cpp)
// which is an exact equality against an always-even number. So the engine
// wants a doubled-row Y, and one that is EVEN, or the comparison can never
// hold. Scaling against the displayed line count and doubling gives that.
// Scaling against the full crop height -- as this did before -- produced an
// odd value half the time, and no shot at those positions could ever register.
static inline int SugarboxGunBufferY(int16_t gun_y, int crop_h, int crop_y)
{
   const int displayed = ((int)gun_y + 0x8000) * SugarboxOutputHeight(crop_h) / 0x10000;
   return displayed * 2 + crop_y;
}
