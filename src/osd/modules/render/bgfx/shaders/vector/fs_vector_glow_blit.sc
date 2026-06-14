$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Additive glow composite WITH the chain's defocus blur baked in. The halation ring / inner fill /
// analytic glow are drawn into a separate FBO and composited after the shadow mask (so the mask does
// not pattern them). But that path skips the hlsl/defocus pass, which previously softened the ring.
// To keep the look, this samples the glow with the SAME 9-tap kernel as chains/hlsl/fs_defocus
// (offsets x u_glow_defocus x 1/1024, resolution-independent), then adds it. u_glow_defocus is bound
// to the chain's `defocus` slider; 0 -> all taps collapse to centre = no blur.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_blit_intensity;
uniform vec4 u_glow_defocus;   // (defocus_x, defocus_y, 0, 0)

void main()
{
	const vec2 C1 = vec2(-1.60,  0.25);
	const vec2 C2 = vec2(-1.00, -0.55);
	const vec2 C3 = vec2(-0.55,  1.00);
	const vec2 C4 = vec2(-0.25, -1.60);
	const vec2 C5 = vec2( 0.25,  1.60);
	const vec2 C6 = vec2( 0.55, -1.00);
	const vec2 C7 = vec2( 1.00,  0.55);
	const vec2 C8 = vec2( 1.60, -0.25);

	vec2 D = u_glow_defocus.xy * vec2_splat(1.0 / 1024.0);

	vec4 s = texture2D(s_tex, v_texcoord0)
		   + texture2D(s_tex, v_texcoord0 + C1 * D)
		   + texture2D(s_tex, v_texcoord0 + C2 * D)
		   + texture2D(s_tex, v_texcoord0 + C3 * D)
		   + texture2D(s_tex, v_texcoord0 + C4 * D)
		   + texture2D(s_tex, v_texcoord0 + C5 * D)
		   + texture2D(s_tex, v_texcoord0 + C6 * D)
		   + texture2D(s_tex, v_texcoord0 + C7 * D)
		   + texture2D(s_tex, v_texcoord0 + C8 * D);

	gl_FragColor = (s / 9.0) * u_blit_intensity.x * v_color0;
}
