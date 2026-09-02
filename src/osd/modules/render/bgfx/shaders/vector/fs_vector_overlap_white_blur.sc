$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// One direction of the direct overlap-white field. Density RGB now uses a separate multiresolution
// pyramid; keeping only A here prevents a wide optical halo from spreading direct white conversion.

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_tex_size0;
uniform vec4 u_overlap_blur_direction;
uniform vec4 u_overlap_white_spread;
uniform vec4 u_vec_res_scale;

void main()
{
// Slider values that name a distance are calibrated in pixels at a 1920-wide reference,
// the same convention as the renderer's vec_res_scale(). Multiplying by it here keeps the
// distance a constant FRACTION of the picture: without it the value means texels, and a
// setting tuned in a small window spreads much further once the window grows. The renderer
// injects the real factor; the effect default of 1.0 leaves any other chain as it was.
	vec2 wtexel = u_overlap_blur_direction.xy
		* (max(u_overlap_white_spread.x, 0.0) * max(u_vec_res_scale.x, 0.01))
		/ max(u_tex_size0.xy, vec2_splat(1.0));
	float white_heat = texture2D(s_tex, v_texcoord0).a * 0.22702703;
	white_heat += (texture2D(s_tex, v_texcoord0 + wtexel * 1.38461538).a
		+ texture2D(s_tex, v_texcoord0 - wtexel * 1.38461538).a) * 0.31621622;
	white_heat += (texture2D(s_tex, v_texcoord0 + wtexel * 3.23076923).a
		+ texture2D(s_tex, v_texcoord0 - wtexel * 3.23076923).a) * 0.07027027;
	gl_FragColor = vec4(0.0, 0.0, 0.0, white_heat) * v_color0;
}
