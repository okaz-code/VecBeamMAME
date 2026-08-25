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
//     0.3745 cancelled the former bloom composite's 2.67 brightness gain for a net 0.99991 add;
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
SAMPLER2D(s_cur, 2);   // the CURRENT excitation frame (same "internal" page the update pass read)

// Scales the post-pool aux buffers (analytic glow, halation, no-persist dots, rays) by the beam
// window's deposited fraction. The renderer used to bake this into their vertices, but their
// content only changes when a new source pass arrives, so it is built once per pass now and the
// per-present ramp arrives here instead. It has to be applied at the sample, ahead of any tonal
// reshape - scaling after a power curve is not the same scaling. 1 outside the window path.
uniform vec4 u_aux_ramp;
#define AUX_TEX2D(_sampler, _uv) (texture2D(_sampler, _uv) * u_aux_ramp.x)

uniform vec4 u_phos;              // y = half_ms (tau), z = curve (p), w = total_ms
uniform vec4 u_phos2;             // x = energy-decay k (0 = off, uniform), y = hold_ms (no decay for this long)
uniform vec4 u_phos_rgb;          // rgb = per-channel half-life multiplier; 1,1,1 = uniform
uniform vec4 u_np_gain;           // (gain, 0, 0, 0): cap_no_persist slider, 0 = off
uniform vec4 u_line_channel_gain; // (r, g, b, 0): phosphor_color slider
                                  // peak (no decay/np), 2 = pool age map (white = 50 ms), 3 = decayed
                                  // pool display without the np add, 4 = the CURRENT excitation frame
                                  // only (shows how much of the content this present actually drew -
                                  // beam-window slicing on busy scenes appears directly here)

float phos_S(float age, float tau, float p, float total)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	float s1 = 1.0 / (1.0 + pow(total / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

// Two-phase energy decay for one channel: the normal part (<=1) decays at the base half-life; the
// overrange excess (>1) decays 'accel' times faster. Summed. This suppresses a hot deposit's extra
// brightness in the afterimage WITHOUT hollowing the feature - the normal part is monotonic in the
// stored peak, so a bright centre never decays below its dimmer edge (no centre-first ring).
float phos_two(float age, float tauN, float p, float totN, float accel, float norm, float over)
{
	return norm * phos_S(age, tauN, p, totN) + over * phos_S(age, tauN / accel, p, totN / accel);
}

void main()
{
	vec4 pool = texture2D(s_tex, v_texcoord0);
	// accel = how much faster the overrange excess decays (u_phos2.x = phosphor_energy_decay).
	// Per-channel (RGB) decay: each channel its own base half-life / total via u_phos_rgb (1,1,1 =
	// uniform). Must match the update pass so re-excite and display agree.
	float accel = 1.0 + max(0.0, u_phos2.x);
	vec3 tauN = u_phos.y * u_phos_rgb.rgb;
	vec3 totN = u_phos.w * u_phos_rgb.rgb;
	vec3 over = max(vec3_splat(0.0), pool.rgb - 1.0);
	vec3 norm = pool.rgb - over;
	// Hold-then-decay (u_phos2.y = hold_ms): full brightness for hold_ms before the decay curve
	// starts - closes the flickering dark seams a short half-life carved between a slowly-moving
	// bright line's successive positions. Must match fs_vector_phosphor (the update pass).
	float ageE = max(0.0, pool.a - u_phos2.y);
	vec3 lit = vec3(
		phos_two(ageE, tauN.r, u_phos.z, totN.r, accel, norm.r, over.r),
		phos_two(ageE, tauN.g, u_phos.z, totN.g, accel, norm.g, over.g),
		phos_two(ageE, tauN.b, u_phos.z, totN.b, accel, norm.b, over.b));
	// Superposition lower bound: a pixel the beam is exciting RIGHT NOW can never be darker than
	// that excitation, whatever the pool's (peak, age) state machine currently holds. Wherever the
	// re-excite hysteresis mis-tracks (fluctuating deposits, overrange residues, scaling glyphs),
	// the error could only show as live content displayed at a DECAYED level - taking max with the
	// current frame makes that entire failure class invisible by construction. In the normal case
	// (pixel re-excited this present) pool == cur and this is exactly the previous output.
	lit = max(lit, texture2D(s_cur, v_texcoord0).rgb);

	lit += AUX_TEX2D(s_np, v_texcoord0).rgb * u_np_gain.x;
	gl_FragColor = vec4(lit * u_line_channel_gain.rgb, 1.0);
}
