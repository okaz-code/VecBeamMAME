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

void main()
{
	vec2 wtexel = u_overlap_blur_direction.xy * max(u_overlap_white_spread.x, 0.0)
		/ max(u_tex_size0.xy, vec2_splat(1.0));
	float white_heat = texture2D(s_tex, v_texcoord0).a * 0.22702703;
	white_heat += (texture2D(s_tex, v_texcoord0 + wtexel * 1.38461538).a
		+ texture2D(s_tex, v_texcoord0 - wtexel * 1.38461538).a) * 0.31621622;
	white_heat += (texture2D(s_tex, v_texcoord0 + wtexel * 3.23076923).a
		+ texture2D(s_tex, v_texcoord0 - wtexel * 3.23076923).a) * 0.07027027;
	gl_FragColor = vec4(0.0, 0.0, 0.0, white_heat) * v_color0;
}
