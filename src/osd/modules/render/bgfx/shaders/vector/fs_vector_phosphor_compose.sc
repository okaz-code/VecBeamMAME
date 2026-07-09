$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Phosphor COMPOSE pass - fuses the former "Phosphor Apply" (pool emit), "NoPersist Combine"
// (no-persist FBO add) and "Phosphor Tint" (channel gain) trio into ONE full-res pass (two full-res
// passes saved per frame; the math is unchanged):
//
//   internal = (pool_peak * S(age) + np * u_np_gain) * u_line_channel_gain
//
//   - S is the same Hill sigmoid as fs_vector_phosphor_emit (S(0)=1, S(half)=0.5, 0 at total_ms).
//   - np is the no-persist FBO (line caps / short-dwell junction dots: bright while drawn, no
//     afterimage, never fed into the glow cascade). The old NoPersist Combine pass's bloom_scale
//     0.3745 cancelled vector_bloom_apply's BLOOM_BRIGHTNESS_GAIN 2.67 for a net 0.99991 add;
//     here the add is exactly 1.0 (that was the intent).
//   - u_np_gain carries the cap_no_persist slider (1 = on, 0 = off). 0 reproduces the old pass's
//     disablewhen skip exactly, and also guards against reading stale np content while the renderer
//     is not drawing/clearing that FBO.
//   - the former tint pass's 3x3 matrix was a fixed identity literal in every phosphor chain, so
//     only the post-mix channel gain (phosphor_color slider) survives here.
//   - no trailing 0..1 clamp: "internal" carries HDR overrange (hot dwell dots deposit multiples of
//     peak) all the way to the HDR-present roll-off/PQ encode, same as the passes this replaces.
//
// u_phos = (dt_ms, half_ms, curve, total_ms) injected by the renderer into the "Phosphor Apply"
// chain entry BY NAME - this pass keeps that entry name; only half/curve/total are used here.

#include "common.sh"

SAMPLER2D(s_tex, 0);   // pool: rgb = peak colour, a = age (ms)
SAMPLER2D(s_np,  1);   // no-persist FBO (caps / junction dots)

uniform vec4 u_phos;              // y = half_ms (tau), z = curve (p), w = total_ms
uniform vec4 u_phos2;             // x = energy-decay k: bright peaks decay faster (0 = off, uniform)
uniform vec4 u_phos_rgb;          // rgb = per-channel half-life multiplier; 1,1,1 = uniform
uniform vec4 u_np_gain;           // (gain, 0, 0, 0): cap_no_persist slider, 0 = off
uniform vec4 u_line_channel_gain; // (r, g, b, 0): phosphor_color slider

float phos_S(float age, float tau, float p, float total)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	float s1 = 1.0 / (1.0 + pow(total / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

void main()
{
	vec4 pool = texture2D(s_tex, v_texcoord0);
	// Energy-dependent decay: brighter stored peak -> shorter effective half-life / total (see u_phos2);
	// must match the update pass so re-excite and display agree.
	// Energy decay keyed on OVERRANGE (peak past 0..1), not raw peak, so a normal-bright thick line
	// decays uniformly instead of hollowing out centre-first (must match the update pass).
	float dmult = 1.0 + max(0.0, u_phos2.x) * max(0.0, max(max(pool.r, pool.g), pool.b) - 1.0);
	// Per-channel (RGB) decay: shared age, each channel its own half-life / total via u_phos_rgb
	// (1,1,1 = uniform). Must match the update pass.
	vec3 tau3 = (u_phos.y / dmult) * u_phos_rgb.rgb;
	vec3 tot3 = (u_phos.w / dmult) * u_phos_rgb.rgb;
	vec3 lit = pool.rgb * vec3(phos_S(pool.a, tau3.r, u_phos.z, tot3.r),
							   phos_S(pool.a, tau3.g, u_phos.z, tot3.g),
							   phos_S(pool.a, tau3.b, u_phos.z, tot3.b));
	lit += texture2D(s_np, v_texcoord0).rgb * u_np_gain.x;
	gl_FragColor = vec4(lit * u_line_channel_gain.rgb, 1.0);
}
