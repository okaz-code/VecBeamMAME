$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_overlay_blur;    // xy = texel direction, z = radius in pixels
uniform vec4 u_overlay_blur_w0; // half-kernel tap weights 0..3
uniform vec4 u_overlay_blur_w1; // x = half-kernel tap weight 4

void main()
{
	vec2 step_uv = u_overlay_blur.xy * u_overlay_blur.z;

	// The nine-tap profile is supplied by the renderer, already normalised to sum to one over
	// the symmetric kernel.  Building it on the CPU keeps the shape control free per pixel and
	// makes the normalisation exact for any curve, which a per-pixel exp()/pow() would not.
	vec3 value = texture2D(s_tex, v_texcoord0).rgb * u_overlay_blur_w0.x;
	value += (texture2D(s_tex, v_texcoord0 + step_uv).rgb
		+ texture2D(s_tex, v_texcoord0 - step_uv).rgb) * u_overlay_blur_w0.y;
	value += (texture2D(s_tex, v_texcoord0 + step_uv * 2.0).rgb
		+ texture2D(s_tex, v_texcoord0 - step_uv * 2.0).rgb) * u_overlay_blur_w0.z;
	value += (texture2D(s_tex, v_texcoord0 + step_uv * 3.0).rgb
		+ texture2D(s_tex, v_texcoord0 - step_uv * 3.0).rgb) * u_overlay_blur_w0.w;
	value += (texture2D(s_tex, v_texcoord0 + step_uv * 4.0).rgb
		+ texture2D(s_tex, v_texcoord0 - step_uv * 4.0).rgb) * u_overlay_blur_w1.x;
	gl_FragColor = vec4(value, 1.0);
}
