$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Colour-vector phosphor compose.  Extends the shared compose with exact no-persist
// gun convergence and the following CIE phosphor-chromaticity conversion, removing
// two full-resolution intermediate passes and their npglow_dc target.

#include "common.sh"

SAMPLER2D(s_tex, 0);   // pool: rgb = peak colour, a = age (ms)
SAMPLER2D(s_np,  1);   // no-persist FBO (caps / junction dots)
SAMPLER2D(s_cur, 2);   // current excitation frame

uniform vec4 u_phos;
uniform vec4 u_phos2;
uniform vec4 u_phos_rgb;
uniform vec4 u_np_gain;
uniform vec4 u_line_channel_gain;
uniform vec4 u_phos_debug;

uniform vec4 u_source_size;
uniform vec4 u_converge_red;
uniform vec4 u_converge_green;
uniform vec4 u_converge_blue;
uniform vec4 u_radial_converge_red;
uniform vec4 u_radial_converge_green;
uniform vec4 u_radial_converge_blue;

uniform vec4 u_y_gain;
uniform vec4 u_chroma_a;
uniform vec4 u_chroma_b;
uniform vec4 u_chroma_c;

float phos_S(float age, float tau, float p, float total)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	float s1 = 1.0 / (1.0 + pow(total / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

float phos_two(float age, float tauN, float p, float totN, float accel, float norm, float over)
{
	return norm * phos_S(age, tauN, p, totN) + over * phos_S(age, tauN / accel, p, totN / accel);
}

vec3 sample_np_converged(vec2 uv)
{
	float amount =
		dot(abs(u_converge_red.xy), vec2_splat(1.0)) +
		dot(abs(u_converge_green.xy), vec2_splat(1.0)) +
		dot(abs(u_converge_blue.xy), vec2_splat(1.0)) +
		dot(abs(u_radial_converge_red.xy), vec2_splat(1.0)) +
		dot(abs(u_radial_converge_green.xy), vec2_splat(1.0)) +
		dot(abs(u_radial_converge_blue.xy), vec2_splat(1.0));

	if (amount <= 0.0)
		return texture2D(s_np, uv).rgb;

	vec2 half_value = vec2_splat(0.5);
	vec2 inv_source = vec2_splat(1.0) / u_source_size.xy;
	vec2 uv_r = (uv - half_value) * (1.0 + u_radial_converge_red.xy) + half_value + u_converge_red.xy * inv_source;
	vec2 uv_g = (uv - half_value) * (1.0 + u_radial_converge_green.xy) + half_value + u_converge_green.xy * inv_source;
	vec2 uv_b = (uv - half_value) * (1.0 + u_radial_converge_blue.xy) + half_value + u_converge_blue.xy * inv_source;
	return vec3(texture2D(s_np, uv_r).r, texture2D(s_np, uv_g).g, texture2D(s_np, uv_b).b);
}

vec3 phosphor_to_srgb(vec3 cin)
{
	mat3 xy = mat3(u_chroma_a.xyz, u_chroma_b.xyz, u_chroma_c.xyz);
	mat3 XYZ_TO_sRGB = mtxFromRows3(
		vec3( 3.2406, -1.5372, -0.4986),
		vec3(-0.9689,  1.8758,  0.0415),
		vec3( 0.0557, -0.2040,  1.0570));
	vec3 cout = vec3_splat(0.0);
	for (int i = 0; i < 3; ++i)
	{
		float Y = u_y_gain[i] * cin[i];
		float X = xy[i].x / xy[i].y * Y;
		float Z = (1.0 - xy[i].x - xy[i].y) / xy[i].y * Y;
		cout += mul(XYZ_TO_sRGB, vec3(X, Y, Z));
	}
	return cout;
}

void main()
{
	vec4 pool = texture2D(s_tex, v_texcoord0);
	float accel = 1.0 + max(0.0, u_phos2.x);
	vec3 tauN = u_phos.y * u_phos_rgb.rgb;
	vec3 totN = u_phos.w * u_phos_rgb.rgb;
	vec3 over = max(vec3_splat(0.0), pool.rgb - 1.0);
	vec3 norm = pool.rgb - over;
	float ageE = max(0.0, pool.a - u_phos2.y);
	vec3 lit = vec3(
		phos_two(ageE, tauN.r, u_phos.z, totN.r, accel, norm.r, over.r),
		phos_two(ageE, tauN.g, u_phos.z, totN.g, accel, norm.g, over.g),
		phos_two(ageE, tauN.b, u_phos.z, totN.b, accel, norm.b, over.b));

	vec3 composed;
	if (u_phos_debug.x > 0.5)
	{
		if (u_phos_debug.x < 1.5)
			composed = pool.rgb;
		else if (u_phos_debug.x < 2.5)
			composed = vec3_splat(pool.a / 50.0);
		else if (u_phos_debug.x < 3.5)
			composed = lit * u_line_channel_gain.rgb;
		else
			composed = texture2D(s_cur, v_texcoord0).rgb;
	}
	else
	{
		lit = max(lit, texture2D(s_cur, v_texcoord0).rgb);
		lit += sample_np_converged(v_texcoord0) * u_np_gain.x;
		composed = lit * u_line_channel_gain.rgb;
	}

	gl_FragColor = vec4(phosphor_to_srgb(composed), 1.0) * v_color0;
}
