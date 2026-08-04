$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_overlay_blur; // xy = texel direction, z = radius in pixels

void main()
{
	vec2 step_uv = u_overlay_blur.xy * u_overlay_blur.z;
	vec3 value = texture2D(s_tex, v_texcoord0).rgb * 0.2270270270;
	value += texture2D(s_tex, v_texcoord0 + step_uv * 1.0).rgb * 0.1945945946;
	value += texture2D(s_tex, v_texcoord0 - step_uv * 1.0).rgb * 0.1945945946;
	value += texture2D(s_tex, v_texcoord0 + step_uv * 2.0).rgb * 0.1216216216;
	value += texture2D(s_tex, v_texcoord0 - step_uv * 2.0).rgb * 0.1216216216;
	value += texture2D(s_tex, v_texcoord0 + step_uv * 3.0).rgb * 0.0540540541;
	value += texture2D(s_tex, v_texcoord0 - step_uv * 3.0).rgb * 0.0540540541;
	value += texture2D(s_tex, v_texcoord0 + step_uv * 4.0).rgb * 0.0162162162;
	value += texture2D(s_tex, v_texcoord0 - step_uv * 4.0).rgb * 0.0162162162;
	gl_FragColor = vec4(value, 1.0);
}
