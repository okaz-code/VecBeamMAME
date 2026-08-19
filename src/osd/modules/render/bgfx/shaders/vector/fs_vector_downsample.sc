$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Vector glow pyramid downsample.
//
// Two kernels, selected per pass by u_ds_kernel.x:
//   0 = legacy 4-tap bilinear box (each tap bilinear-samples a 2x2 source block). Cheapest, but a box
//       average turns a thin diagonal line into an axis-aligned staircase, and six levels of that make
//       the far halo read as square-ish beads however smoothly it is reconstructed afterwards.
//   1 = 13-tap tent (the COD:AW / Jimenez bloom downsample): a centre 2x2 group plus four overlapping
//       2x2 groups on the 3x3 lattice. Same unit total weight - so line energy is still preserved -
//       but the overlapping groups remove the staircase and the deep levels carry a round halo.
//
// u_glow_curve / u_glow_pivot reshape the source tonally BEFORE the average, and are bound on one
// pass only (ds0 in the vector chains). The average is what erases the difference between "one bright compact core" and
// "a broad block of moderate strokes", so a curve applied after it - or after the whole pyramid, like
// the combine's shape_glow - cannot tell those apart. Per tap, hue-preserving on the peak channel:
//   peak' = pivot * (peak / pivot)^curve
// curve > 1 therefore suppresses everything below the pivot and amplifies everything above it, while
// the pivot itself is a fixed point (so the calibration at that one level is unchanged). The pivot has
// to sit ABOVE the content being suppressed to do anything: content sitting exactly at the pivot is
// unchanged no matter how large the curve is.
// curve = 1 is identity and is the default: chains that do not bind the uniforms are untouched.
//
// The chain system sets u_inv_screen_dims to the reciprocal size of the first input texture.

#include "common.sh"

SAMPLER2D(s_tex, 0);

// set automatically by the chain system from the input texture size: (1/input_w, 1/input_h, 0, 0)
uniform vec4 u_inv_screen_dims;
uniform vec4 u_glow_curve;   // (curve, 0, 0, 0); 1 = identity
uniform vec4 u_glow_pivot;   // (pivot, 0, 0, 0); the fixed point of the curve
uniform vec4 u_ds_kernel;    // (kernel, 0, 0, 0); 0 = legacy box, 1 = 13-tap tent

// Safety ceiling on the tonal reshape: an unbounded power curve on an HDR source can push a hot texel
// far past the useful range of the float target and make the halo pump frame to frame. 32x linear is
// well above any setting that still reads as glow.
#define GLOW_SHAPE_MAX_BOOST 32.0

vec4 shape_tap(vec4 c)
{
	float curve = u_glow_curve.x;
	if (curve == 1.0) return c;
	vec3 x = max(c.rgb, vec3_splat(0.0));
	float peak = max(x.r, max(x.g, x.b));
	if (peak <= 1.0e-7) return vec4(vec3_splat(0.0), c.a);
	float pivot = max(u_glow_pivot.x, 1.0e-4);
	float shaped = min(pivot * pow(peak / pivot, curve), peak * GLOW_SHAPE_MAX_BOOST);
	// Scale all channels by one factor so the reshape changes brightness, not hue. Alpha (coverage)
	// stays linear - only the wide path reads these targets and it reads rgb.
	return vec4(x * (shaped / peak), c.a);
}

vec4 tap(vec2 uv)
{
	return shape_tap(texture2D(s_tex, uv));
}

void main()
{
	vec2 o = u_inv_screen_dims.xy;
	vec2 uv = v_texcoord0;
	vec4 c;
	if (u_ds_kernel.x < 0.5)
	{
		// legacy box: 4 bilinear taps on the half-texel diagonals
		vec2 h = o * 0.5;
		c = (tap(uv + vec2(-h.x, -h.y))
		   + tap(uv + vec2( h.x, -h.y))
		   + tap(uv + vec2(-h.x,  h.y))
		   + tap(uv + vec2( h.x,  h.y))) * 0.25;
	}
	else
	{
		// 13-tap tent. Offsets are in SOURCE texels, so +-2 reaches one output texel.
		vec4 A = tap(uv + vec2(-2.0 * o.x, -2.0 * o.y));
		vec4 B = tap(uv + vec2(       0.0, -2.0 * o.y));
		vec4 C = tap(uv + vec2( 2.0 * o.x, -2.0 * o.y));
		vec4 D = tap(uv + vec2(      -o.x,       -o.y));
		vec4 E = tap(uv + vec2(       o.x,       -o.y));
		vec4 F = tap(uv + vec2(-2.0 * o.x,        0.0));
		vec4 G = tap(uv);
		vec4 H = tap(uv + vec2( 2.0 * o.x,        0.0));
		vec4 I = tap(uv + vec2(      -o.x,        o.y));
		vec4 J = tap(uv + vec2(       o.x,        o.y));
		vec4 K = tap(uv + vec2(-2.0 * o.x,  2.0 * o.y));
		vec4 L = tap(uv + vec2(       0.0,  2.0 * o.y));
		vec4 M = tap(uv + vec2( 2.0 * o.x,  2.0 * o.y));
		c = G * 0.125
		  + (A + C + K + M) * 0.03125
		  + (B + F + H + L) * 0.0625
		  + (D + E + I + J) * 0.125;
	}
	gl_FragColor = c * v_color0;
}
