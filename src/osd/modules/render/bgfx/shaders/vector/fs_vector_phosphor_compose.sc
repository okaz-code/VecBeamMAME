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
SAMPLER2D(s_age, 3);  // pool attachment 1: rgb = per-channel age (ms)
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
                                  // z = strike flash ms (0 = off), w = flash gain
uniform vec4 u_phos_rgb;          // rgb = per-channel half-life multiplier; 1,1,1 = uniform
// Fresh-excitation gain. Halation and glass scatter follow INSTANTANEOUS intensity, and the two
// sides of this max() are not alike in that respect: the spot being written is deposited in tens of
// microseconds at full beam current, while the pool residue emits its (lower) light spread over the
// whole frame. Equal frame-average values therefore do not mean equal spot brightness, and a bullet
// trail whose head sits inside phosphor_hold_ms reads exactly as bright as the dot behind it.
// 1.0 = the previous behaviour.
// Pool-side ceiling (masked_core_peak). The pool holds the beam PEAK, so a spot the beam
// dwelled on can sit far above one calibrated beam; this caps what the fresh sample is
// allowed to contribute, matching the same limit the update pass applies before storing.
// 0 = off, which is what the monochrome chain did before it had the slider.
uniform vec4 u_phos_peak;
uniform vec4 u_fresh_gain;
uniform vec4 u_np_gain;           // (gain, 0, 0, 0): cap_no_persist slider, 0 = off
uniform vec4 u_line_channel_gain; // (r, g, b, 0): phosphor_color slider
                                  // peak (no decay/np), 2 = pool age map (white = 50 ms), 3 = decayed
                                  // pool display without the np add, 4 = the CURRENT excitation frame
                                  // only (shows how much of the content this present actually drew -
                                  // beam-window slicing on busy scenes appears directly here)

float phos_S(float age, float tau, float p, float total, float s1)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

// Two-phase energy decay for one channel: the normal part (<=1) decays at the base half-life; the
// overrange excess (>1) decays 'accel' times faster. Summed. This suppresses a hot deposit's extra
// brightness in the afterimage WITHOUT hollowing the feature - the normal part is monotonic in the
// stored peak, so a bright centre never decays below its dimmer edge (no centre-first ring).
float phos_two(float age, float tauN, float p, float totN, float accel, float norm, float over)
{
	float tauA = tauN / accel;
	float totA = totN / accel;
	// The residual at total is the same for both terms: the overdrive acceleration divides total and
	// tau together, so their ratio survives it. The only way it does not is the 0.001 floor biting on
	// the accelerated tau, which needs a half-life under a microsecond - computed properly there
	// rather than assumed away. Everywhere else this is one pow per channel instead of two.
	float s1  = 1.0 / (1.0 + pow(totN / max(tauN, 0.001), p));
	float s1a = (tauA >= 0.001) ? s1 : (1.0 / (1.0 + pow(totA / max(tauA, 0.001), p)));
	return norm * phos_S(age, tauN, p, totN, s1) + over * phos_S(age, tauA, p, totA, s1a);
}

// Strike flash (u_phos2.z = flash_ms, .w = gain): for the first flash_ms after a pixel is excited
// it emits ABOVE the decay curve - the fast initial component that makes the spot the beam has
// just crossed the brightest thing on the tube. Multiplies the pool emission (and the current
// excitation it is floored to), never the no-persist add: that buffer carries no age, is already a
// drawn-this-present route, and boosting it would count the same light twice.
//
// Beam-window only - the renderer injects 0 for .z otherwise. Without the window every present
// re-deposits the whole pass, so every pixel sits at age 0 and the "flash" degenerates into a flat
// gain on everything. Age advances one present at a time, so a flash_ms below the present interval
// means exactly "the slice deposited this present"; that is the intended reading of small values,
// not a rounding failure. Larger values reach back over earlier presents, linearly.
float phos_flash(float age)
{
	if (u_phos2.z <= 0.0) return 1.0;
	return 1.0 + max(0.0, u_phos2.w - 1.0) * clamp(1.0 - age / u_phos2.z, 0.0, 1.0);
}

void main()
{
	vec3 pool = texture2D(s_tex, v_texcoord0).rgb;
	// One age per channel, matching the update pass: channels excited at different times
	// decay from their own excitation instead of sharing the most recent one.
	vec3 poolAge = texture2D(s_age, v_texcoord0).rgb;
	vec3 fresh = texture2D(s_cur, v_texcoord0).rgb;
	float raw_fresh_peak = max(fresh.r, max(fresh.g, fresh.b));
	if (u_phos_peak.x > 0.0 && raw_fresh_peak > u_phos_peak.x)
		fresh *= u_phos_peak.x / raw_fresh_peak;
	// accel = how much faster the overrange excess decays (u_phos2.x = phosphor_energy_decay).
	// Per-channel (RGB) decay: each channel its own base half-life / total via u_phos_rgb (1,1,1 =
	// uniform). Must match the update pass so re-excite and display agree.
	float accel = 1.0 + max(0.0, u_phos2.x);
	vec3 tauN = u_phos.y * u_phos_rgb.rgb;
	vec3 totN = u_phos.w * u_phos_rgb.rgb;
	vec3 over = max(vec3_splat(0.0), pool - 1.0);
	vec3 norm = pool - over;
	// Hold-then-decay (u_phos2.y = hold_ms): full brightness for hold_ms before the decay curve
	// starts - closes the flickering dark seams a short half-life carved between a slowly-moving
	// bright line's successive positions. Must match fs_vector_phosphor (the update pass).
	vec3 ageE = max(vec3_splat(0.0), poolAge - vec3_splat(u_phos2.y));
	vec3 lit = vec3(
		phos_two(ageE.r, tauN.r, u_phos.z, totN.r, accel, norm.r, over.r),
		phos_two(ageE.g, tauN.g, u_phos.z, totN.g, accel, norm.g, over.g),
		phos_two(ageE.b, tauN.b, u_phos.z, totN.b, accel, norm.b, over.b));
	// Superposition lower bound: a pixel the beam is exciting RIGHT NOW can never be darker than
	// that excitation, whatever the pool's (peak, age) state machine currently holds. Wherever the
	// re-excite hysteresis mis-tracks (fluctuating deposits, overrange residues, scaling glyphs),
	// the error could only show as live content displayed at a DECAYED level - taking max with the
	// current frame makes that entire failure class invisible by construction. In the normal case
	// (pixel re-excited this present) pool == cur and this is exactly the previous output.
	lit = max(lit, fresh);
	// Fresh-excitation gain, applied in the pool's own scale rather than to the raw s_cur sample -
	// the two are not commensurate (measured about 5:1 on asteroid, and more before the roll-off), so
	// a gain on s_cur has to exceed that ratio before max() even notices it. age == 0 means the pixel
	// was excited THIS present, which is exactly the newest hit and nothing else.
	if (u_fresh_gain.x > 1.0 && min(poolAge.r, min(poolAge.g, poolAge.b)) <= 0.0)
		lit *= u_fresh_gain.x;
	lit *= phos_flash(min(poolAge.r, min(poolAge.g, poolAge.b)));

	lit += AUX_TEX2D(s_np, v_texcoord0).rgb * u_np_gain.x;
	gl_FragColor = vec4(lit * u_line_channel_gain.rgb, 1.0);
}
