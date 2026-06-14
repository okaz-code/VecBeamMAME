$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// 13-tap Gaussian upsample for the ladder bloom.
// Samples the lower-resolution acc(n+1) with a ~sigma 1.3 (source-texel) gaussian out to +-2
// texels and adds the result into acc(n) under ADD blend (blend state from vector_upsample.json).
// The old 9-tap 1-2-1 tent only reached +-1 texel, so reconstructing a very low-res deep level by a
// large factor left piecewise-linear facets (diagonal block banding) in the wide glow. Widening the
// support to +-2 with gaussian weights smooths those facets out while staying one pass / cheap.
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
	vec2 uv = v_texcoord0;

	// gaussian sigma ~1.6 source texels: center 1.0, edge(+-1) 0.822, corner(+-1,+-1) 0.677, far(+-2) 0.458
	vec4 c = texture2D(s_tex, uv) * 1.0;
	c += (texture2D(s_tex, uv + vec2(-o.x, 0.0))
		+ texture2D(s_tex, uv + vec2( o.x, 0.0))
		+ texture2D(s_tex, uv + vec2(0.0, -o.y))
		+ texture2D(s_tex, uv + vec2(0.0,  o.y))) * 0.822;
	c += (texture2D(s_tex, uv + vec2(-o.x, -o.y))
		+ texture2D(s_tex, uv + vec2( o.x, -o.y))
		+ texture2D(s_tex, uv + vec2(-o.x,  o.y))
		+ texture2D(s_tex, uv + vec2( o.x,  o.y))) * 0.677;
	c += (texture2D(s_tex, uv + vec2(-2.0 * o.x, 0.0))
		+ texture2D(s_tex, uv + vec2( 2.0 * o.x, 0.0))
		+ texture2D(s_tex, uv + vec2(0.0, -2.0 * o.y))
		+ texture2D(s_tex, uv + vec2(0.0,  2.0 * o.y))) * 0.458;

	gl_FragColor = c * (1.0 / 8.828) * u_blit_intensity.x * v_color0;
}
