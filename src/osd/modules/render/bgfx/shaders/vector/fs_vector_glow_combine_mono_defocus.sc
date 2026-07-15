$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Fused monochrome-vector post-phosphor stage. This preserves the old order
// defocus -> asymmetric vector pincushion -> ambient -> glow without a
// full-resolution intermediate image or a needless shadow-mask texture sample.

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);

uniform vec4 u_defocus;
uniform vec4 u_vec_pincushion_x_quad;
uniform vec4 u_vec_pincushion_x_cubic;
uniform vec4 u_vec_pincushion_y_quad;
uniform vec4 u_vec_pincushion_y_cubic;
uniform vec4 u_glow_enable;
uniform vec4 u_ambient_color;
uniform vec4 u_ambient_level;

#define PINCUSHION_GAIN (5.0 / 30.0)
#define GLOW_BRIGHTNESS_GAIN 2.67

vec2 vector_pincushion_uv(vec2 texcoord)
{
	vec2 uv = texcoord * 2.0 - 1.0;
	float x = uv.x;
	float y = uv.y;
	float y2 = y * y;
	float x2 = x * x;
	float pinch_x = (u_vec_pincushion_x_quad.x + u_vec_pincushion_x_cubic.x * y2) * PINCUSHION_GAIN;
	float pinch_y = (u_vec_pincushion_y_quad.x + u_vec_pincushion_y_cubic.x * x2) * PINCUSHION_GAIN;
	x *= 1.0 + pinch_x * y2;
	y *= 1.0 + pinch_y * x2;
	return (vec2(x, y) + 1.0) * 0.5;
}

vec3 sample_defocused(vec2 uv)
{
	if (u_defocus.x == 0.0 && u_defocus.y == 0.0)
		return texture2D(s_base, uv).rgb;

	const vec2 C1 = vec2(-1.60,  0.25);
	const vec2 C2 = vec2(-1.00, -0.55);
	const vec2 C3 = vec2(-0.55,  1.00);
	const vec2 C4 = vec2(-0.25, -1.60);
	const vec2 C5 = vec2( 0.25,  1.60);
	const vec2 C6 = vec2( 0.55, -1.00);
	const vec2 C7 = vec2( 1.00,  0.55);
	const vec2 C8 = vec2( 1.60, -0.25);
	vec2 d = u_defocus.xy * vec2_splat(1.0 / 1024.0);

	vec3 blurred = texture2D(s_base, uv).rgb
		+ texture2D(s_base, uv + C1 * d).rgb
		+ texture2D(s_base, uv + C2 * d).rgb
		+ texture2D(s_base, uv + C3 * d).rgb
		+ texture2D(s_base, uv + C4 * d).rgb
		+ texture2D(s_base, uv + C5 * d).rgb
		+ texture2D(s_base, uv + C6 * d).rgb
		+ texture2D(s_base, uv + C7 * d).rgb
		+ texture2D(s_base, uv + C8 * d).rgb;
	return blurred * (1.0 / 9.0);
}

void main()
{
	vec2 base_uv = vector_pincushion_uv(v_texcoord0);
	bool outside = base_uv.x < 0.0 || base_uv.x > 1.0 || base_uv.y < 0.0 || base_uv.y > 1.0;
	vec3 base = outside ? vec3_splat(0.0) : sample_defocused(base_uv);
	vec3 ambient = u_ambient_level.x * 0.001 * u_ambient_color.rgb;

	vec3 glow = vec3_splat(0.0);
	if (u_glow_enable.x > 0.0)
		glow = texture2D(s_bloom, v_texcoord0).rgb * GLOW_BRIGHTNESS_GAIN;

	gl_FragColor = vec4(base + ambient + glow, 1.0) * v_color0;
}
