$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Box prefilter in front of the resin diffusion blur.
//
// Without this the blur's first pass was the downsample: it point-sampled the full-resolution
// vector image with taps a whole low-resolution texel apart, which at a scale of four means a gap
// of about seven source pixels.  A one-pixel-wide vector was then hit or missed depending on where
// it fell, and the diffusion showed the sampling comb as a lattice.  Averaging the whole source
// block first band-limits the image so the sparse blur taps sample something smooth.
//
// A bilinear fetch placed on a texel boundary returns the mean of the two texels it straddles, so
// the block is covered exactly by fetching at the odd source-texel positions inside it.  Relative
// to the destination texel centre those sit at +/-1 and +/-3 source texels for scale eight, +/-1
// for scale four, and on the centre itself for scale two.  The renderer supplies the two magnitudes
// and this samples every sign combination of them, giving a 4x4 grid of fetches: 8x8 exactly at
// scale eight, and at the smaller scales the same positions repeat, which leaves the mean exact and
// only wastes fetches on a target that is already small.  Being an exact mean, no energy enters or
// leaves before the diffusion is normalised.

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_overlay_ds; // xy = near tap offset (source UV), zw = far tap offset

// the four sign combinations of one offset pair
#define VXO_TAP4(dx, dy) ( \
	texture2D(s_tex, v_texcoord0 + vec2(-(dx), -(dy))).rgb + \
	texture2D(s_tex, v_texcoord0 + vec2( (dx), -(dy))).rgb + \
	texture2D(s_tex, v_texcoord0 + vec2(-(dx),  (dy))).rgb + \
	texture2D(s_tex, v_texcoord0 + vec2( (dx),  (dy))).rgb)

void main()
{
	float near_x = u_overlay_ds.x;
	float near_y = u_overlay_ds.y;
	float far_x = u_overlay_ds.z;
	float far_y = u_overlay_ds.w;

	vec3 value = VXO_TAP4(near_x, near_y);
	value += VXO_TAP4(far_x, near_y);
	value += VXO_TAP4(near_x, far_y);
	value += VXO_TAP4(far_x, far_y);
	gl_FragColor = vec4(value * 0.0625, 1.0);
}
