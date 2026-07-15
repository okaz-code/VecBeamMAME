$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Colour-vector final glow composite.  Fuses the pre-glow shadow-mask/ambient pass
// with the old neutral vector_bloom_apply invocation while preserving its measured
// 2.67 glow calibration: output = masked(base) + ambient + glow * 2.67.

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);
SAMPLER2D(s_mask,  2);

uniform vec4 u_glow_enable;
uniform vec4 u_shadow_mask_strength;
uniform vec4 u_shadow_mask_brightboost;
uniform vec4 u_shadow_mask_scale;
uniform vec4 u_target_dims;
uniform vec4 u_ambient_color;
uniform vec4 u_ambient_level;
uniform vec4 u_ambient_mask;

#define GLOW_BRIGHTNESS_GAIN 2.67

void main()
{
	vec3 base = texture2D(s_base, v_texcoord0).rgb;

	float strength = clamp(u_shadow_mask_strength.x, 0.0, 1.0);
	float brightboost = clamp(u_shadow_mask_brightboost.x, 0.0, 2.0);
	float slider_scale = max(1.0, u_shadow_mask_scale.x);
	float scale = max(1.0, floor(slider_scale * (u_target_dims.x / 1920.0) + 0.5));
	vec2 mask_uv = v_texcoord0 * u_target_dims.xy / scale;
	vec3 mask = texture2D(s_mask, mask_uv).rgb;
	vec3 mask_factor = mix(vec3_splat(1.0), mask * (1.0 + brightboost), strength);

	vec3 ambient = u_ambient_level.x * 0.001 * u_ambient_color.rgb;
	vec3 ambient_out = ambient * mix(vec3_splat(1.0), mask_factor, clamp(u_ambient_mask.x, 0.0, 1.0));

	vec3 glow = vec3_splat(0.0);
	if (u_glow_enable.x > 0.0)
		glow = texture2D(s_bloom, v_texcoord0).rgb * GLOW_BRIGHTNESS_GAIN;

	gl_FragColor = vec4(base * mask_factor + ambient_out + glow, 1.0) * v_color0;
}
