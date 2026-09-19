#include "gtest/gtest.h"
#include "display_geometry.h"

// The two crop modes the core actually ships: "normal" and "full" border.
struct CropMode { const char* name; int w, h, x, y; };
static const CropMode kModes[] = {
   { "normal", WIDTH,      HEIGHT,      OFFSET_X,      OFFSET_Y      },
   { "full",   FULL_WIDTH, FULL_HEIGHT, FULL_OFFSET_X, FULL_OFFSET_Y },
};

// The formula this core used before, kept here and nowhere else, so the bug
// it encodes is documented and the new behaviour is proven against it rather
// than merely asserted.
static int Legacy_GunBufferY(int16_t gun_y, int crop_h, int crop_y)
{
   return ((int)gun_y + 0x8000) * crop_h / 0x10000 + crop_y;
}

TEST(DisplayGeometry, OutputHeightIsHalfTheCrop)
{
   EXPECT_EQ(280, SugarboxOutputHeight(FULL_HEIGHT));
   EXPECT_EQ(240, SugarboxOutputHeight(HEIGHT));
}

// CRTC.cpp compares "monitor_->y_ * 2 == gun_y_", an exact equality against
// an always-even value. An odd Y can therefore never match, whatever else is
// right. Sweep the entire 16-bit input range in both crop modes.
TEST(DisplayGeometry, GunYIsAlwaysEven)
{
   for (const CropMode& m : kModes)
   {
      for (int v = -32768; v <= 32767; ++v)
      {
         const int y = SugarboxGunBufferY((int16_t)v, m.h, m.y);
         ASSERT_EQ(0, y % 2) << m.name << " gun_y=" << v << " -> " << y;
      }
   }
}

// Parity alone would be satisfied by a constant. Every displayed line must
// also be reachable, or part of the screen is dead to the gun.
TEST(DisplayGeometry, EveryDisplayedLineIsReachable)
{
   for (const CropMode& m : kModes)
   {
      const int rows = SugarboxOutputHeight(m.h);
      std::vector<bool> hit(rows, false);
      for (int v = -32768; v <= 32767; ++v)
      {
         const int y = SugarboxGunBufferY((int16_t)v, m.h, m.y);
         const int row = (y - m.y) / 2;
         ASSERT_GE(row, 0) << m.name;
         ASSERT_LT(row, rows) << m.name << " gun_y=" << v;
         hit[row] = true;
      }
      for (int r = 0; r < rows; ++r)
         ASSERT_TRUE(hit[r]) << m.name << " display row " << r << " unreachable";
   }
}

TEST(DisplayGeometry, GunYSpansTheCropAndIsMonotonic)
{
   for (const CropMode& m : kModes)
   {
      EXPECT_EQ(m.y, SugarboxGunBufferY(-32768, m.h, m.y)) << m.name;
      EXPECT_LT(SugarboxGunBufferY(32767, m.h, m.y), m.y + m.h) << m.name;
      int prev = SugarboxGunBufferY(-32768, m.h, m.y);
      for (int v = -32768; v <= 32767; v += 37)
      {
         const int y = SugarboxGunBufferY((int16_t)v, m.h, m.y);
         ASSERT_GE(y, prev) << m.name;
         prev = y;
      }
   }
}

TEST(DisplayGeometry, GunXSpansTheCrop)
{
   for (const CropMode& m : kModes)
   {
      EXPECT_EQ(m.x, SugarboxGunBufferX(-32768, m.w, m.x)) << m.name;
      EXPECT_LT(SugarboxGunBufferX(32767, m.w, m.x), m.x + m.w) << m.name;
   }
}

// The differential proof: the old formula fails the parity invariant above,
// and fails it for about half of all inputs rather than in some corner.
TEST(DisplayGeometry, LegacyFormulaProducedUnmatchableOddRows)
{
   for (const CropMode& m : kModes)
   {
      int odd = 0;
      for (int v = -32768; v <= 32767; ++v)
         if (Legacy_GunBufferY((int16_t)v, m.h, m.y) % 2 != 0)
            ++odd;
      EXPECT_GT(odd, 30000) << m.name << ": expected roughly half of 65536 to be odd";
   }
}

// The on-screen keyboard is laid out in displayed rows, so it has to fit the
// smaller of the two modes. These are the panel heights from libretro.cpp.
TEST(DisplayGeometry, OskPanelsFitTheShorterBorderMode)
{
   const int rows = SugarboxOutputHeight(HEIGHT);   // 240, the tighter case
   const int command_list_bottom = 20 + 12 * 13 + 12;
   const int grid_bottom         = (30 - 10) + (5 * 25 + 50);
   EXPECT_LE(command_list_bottom, rows);
   EXPECT_LE(grid_bottom, rows);
}
