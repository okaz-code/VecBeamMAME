$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// 4-tap bilinear box downsample for the vector glow.
// Averages all source pixels so line energy is not lost on downsample.
// Each of the 4 taps bilinear-samples a 2x2 source -> covers 4x4 = 16 source pixels total.
//
// The chain system sets u_inv_screen_dims to the reciprocal size of the first input texture.
// The * 0.5 gives the bilinear half-texel offset, so the formula is common to every downsample stage.

#include "common.sh"

SAMPLER2D(s_tex, 0);

// set automatically by the chain system from the input texture size: (1/input_w, 1/input_h, 0, 0)
uniform vec4 u_inv_screen_dims;

void main()
{
	vec2 o = u_inv_screen_dims.xy * 0.5;
	vec4 c = texture2D(s_tex, v_texcoord0 + vec2(-o.x, -o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2( o.x, -o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2(-o.x,  o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2( o.x,  o.y));
	gl_FragColor = c * 0.25 * v_color0;
}
