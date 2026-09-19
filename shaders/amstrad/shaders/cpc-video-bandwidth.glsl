/*
   CPC video bandwidth -- horizontal-only Gaussian.

   Models the limited bandwidth of a 1980s CPC monitor's video amplifier.
   The CPC clocks one CRTC character (2 bytes) per microsecond into 16
   buffer pixels, so in this core's output a Mode 1 pixel is 2 texels wide
   and a Mode 0 pixel is 4. On a real CTM644 you can barely tell Mode 0
   from Mode 1, because the amplifier cannot produce edges that sharp.

   crt-guest's own h_sharp cannot do this: its kernel is exp2(-h_sharp*w*w),
   so even at the h_sharp floor of 1.0 a tap 3 texels out weighs exp2(-9),
   about 0.002. Its reach is under +/-1.5 texels -- less than half a Mode 0
   pixel. Hence a dedicated pass, run before the CRT chain.

   Author: REG-Linux / SugarLibRetro.  License: public domain.
*/

#pragma parameter cpc_bw_sigma "CPC video bandwidth (texels)" 1.60 0.0 4.0 0.05

#if defined(VERTEX)
#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#define COMPAT_TEXTURE texture2D
#endif
#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 COLOR;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 COL0;
COMPAT_VARYING vec4 TEX0;

uniform mat4 MVPMatrix;
uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;

void main()
{
   gl_Position = MVPMatrix * VertexCoord;
   COL0 = COLOR;
   TEX0.xy = TexCoord.xy;
}

#elif defined(FRAGMENT)

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif
#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;
#define vTexCoord TEX0.xy

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float cpc_bw_sigma;
#else
#define cpc_bw_sigma 1.60
#endif

void main()
{
   if (cpc_bw_sigma <= 0.001)
   {
      FragColor = COMPAT_TEXTURE(Texture, vTexCoord);
      return;
   }

   // 11 taps: +/-5 texels covers a whole Mode 0 pixel (4 texels) and then
   // some, so adjacent Mode 0 blocks genuinely bleed into one another.
   float dx = 1.0 / TextureSize.x;
   float s2 = 2.0 * cpc_bw_sigma * cpc_bw_sigma;
   vec3 acc = vec3(0.0);
   float wsum = 0.0;
   for (int i = -5; i <= 5; i++)
   {
      float fi = float(i);
      float w = exp(-(fi * fi) / s2);
      acc += w * COMPAT_TEXTURE(Texture, vec2(vTexCoord.x + fi * dx, vTexCoord.y)).rgb;
      wsum += w;
   }
   FragColor = vec4(acc / wsum, 1.0);
}
#endif
