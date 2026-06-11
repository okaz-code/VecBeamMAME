$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// 9-tap (3x3) tent upsample for the ladder bloom.
// Samples the lower-resolution acc(n+1) with a 1-2-1 / 2-4-2 / 1-2-1 kernel (taps one source
// texel apart) and adds the result into acc(n) under ADD blend (the blend state comes from
// vector_upsample.json). Compared to a single bilinear tap this suppresses the diamond/star
// artifacts that repeated bilinear up-adds can produce.
//
// The chain system sets u_inv_screen_dims to the reciprocal size of the first input texture.

#include "common.sh"

SAMPLER2D(s_tex, 0);

// set automatically by the chain system from the input texture size: (1/input_w, 1/input_h, 0, 0)
uniform vec4 u_inv_screen_dims;
uniform vec4 u_blit_intensity;

void main()
{
	vec2 o = u_inv_screen_dims.xy;

	// corners (weight 1)
	vec4 c = texture2D(s_tex, v_texcoord0 + vec2(-o.x, -o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2( o.x, -o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2(-o.x,  o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2( o.x,  o.y));
	// edges (weight 2)
	c += (texture2D(s_tex, v_texcoord0 + vec2(-o.x, 0.0))
		+ texture2D(s_tex, v_texcoord0 + vec2( o.x, 0.0))
		+ texture2D(s_tex, v_texcoord0 + vec2(0.0, -o.y))
		+ texture2D(s_tex, v_texcoord0 + vec2(0.0,  o.y))) * 2.0;
	// center (weight 4)
	c += texture2D(s_tex, v_texcoord0) * 4.0;

	gl_FragColor = c * (1.0 / 16.0) * u_blit_intensity.x * v_color0;
}
