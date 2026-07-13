$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Colour phosphor-decay pool - UPDATE pass. Per pixel the pool stores rgb = peak excitation colour and
// a = age in ms since that excitation. Each present: if the fresh excitation frame (s_tex) is at least
// as bright as the decayed previous value, the pixel is (re)excited -> peak = cur, age = 0; otherwise
// the peak colour is held and age advances by dt. The displayed value (peak * S(age)) is produced by
// the companion EMIT pass. S is a sigmoid-family (Hill) falloff: S(0)=1, S(half)=0.5, normalised to
// reach 0 at total_ms. dt = 0 (paused) freezes the image. Colour-agnostic, so it works for monochrome
// (rgb equal), the Vectrex 3D imager, and Atari AVG colour vector games alike. Being purely per-pixel
// (age in the pool, not per-vector timestamps) it needs no t0/t1 - even untimed sources (AVG/DVG) work.

#include "common.sh"

SAMPLER2D(s_prev, 0);   // pool, previous frame: rgb = peak colour, a = age (ms)
SAMPLER2D(s_tex,  1);   // current fresh excitation frame (rgb)

uniform vec4 u_phos;    // x = dt_ms, y = half_ms (tau), z = curve (p), w = total_ms
uniform vec4 u_phos2;   // x = energy-decay k (0 = off, uniform), y = hold_ms (no decay for this long)
uniform vec4 u_phos_rgb; // rgb = per-channel half-life multiplier (blue phosphor shorter etc.); 1,1,1 = uniform

float phos_S(float age, float tau, float p, float total)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	float s1 = 1.0 / (1.0 + pow(total / max(tau, 0.001), p));   // residual at total -> subtract so it hits 0
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

// Two-phase energy decay for one channel: normal part (<=1) at the base half-life, overrange excess
// (>1) 'accel' times faster. Summed. Suppresses a hot deposit's extra brightness in the afterimage
// WITHOUT hollowing (the normal part stays monotonic in the stored peak, so a bright centre never
// decays below its dimmer edge). Must match fs_vector_phosphor_compose.
float phos_two(float age, float tauN, float p, float totN, float accel, float norm, float over)
{
	return norm * phos_S(age, tauN, p, totN) + over * phos_S(age, tauN / accel, p, totN / accel);
}

void main()
{
	vec3  cur   = texture2D(s_tex,  v_texcoord0).rgb;
	vec4  prev  = texture2D(s_prev, v_texcoord0);
	vec3  peakP = prev.rgb;
	float ageP  = prev.a;

	// Two-phase energy decay + per-channel (RGB) half-life. accel = overrange-excess speed-up
	// (u_phos2.x = phosphor_energy_decay); u_phos_rgb = per-channel half-life multiplier (1,1,1 =
	// uniform). The re-excite test compares the BRIGHTEST surviving channel to the fresh frame.
	// Must match the compose pass so re-excite and display agree.
	float accel = 1.0 + max(0.0, u_phos2.x);
	vec3 tauN = u_phos.y * u_phos_rgb.rgb;
	vec3 totN = u_phos.w * u_phos_rgb.rgb;
	vec3 overP = max(vec3_splat(0.0), peakP - 1.0);
	vec3 normP = peakP - overP;
	// Hold-then-decay (u_phos2.y = hold_ms): the afterglow stays at full brightness for hold_ms
	// before the decay curve starts. With a short calibrated half-life (trail look), a single
	// present interval already dropped the residue to ~27%, carving flickering dark seams between a
	// slowly-moving bright line's successive positions; the eye integrates ~a frame on the real
	// thing, so holding for about one present closes the seams without lengthening the tail's shape.
	// Must match fs_vector_phosphor_compose.
	float ageE = max(0.0, ageP - u_phos2.y);
	vec3 dRGB = vec3(
		phos_two(ageE, tauN.r, u_phos.z, totN.r, accel, normP.r, overP.r),
		phos_two(ageE, tauN.g, u_phos.z, totN.g, accel, normP.g, overP.g),
		phos_two(ageE, tauN.b, u_phos.z, totN.b, accel, normP.b, overP.b));
	float decayed = max(max(dRGB.r, dRGB.g), dRGB.b);
	float curL    = max(max(cur.r, cur.g), cur.b);

	vec3  peak;
	float age;
	if (curL >= decayed)
	{
		// (Re)excited: take the new excitation, but keep - per channel - any still-brighter decayed
		// residue of the previous deposit. Real phosphor excitation superposes; a winner-take-all
		// replacement (peak = cur) let a colour flank re-hitting a pixel that recently held a bright
		// WHITE (overloaded) core clobber the white afterglow's other channels, carving a dark band
		// through the interior of a slowly moving overloaded line (a static line re-deposits the same
		// profile every frame, so it never showed). max() re-anchors the surviving residue at age 0 -
		// a slight over-persistence, but monotonic and artifact-free.
		peak = max(cur, dRGB);   age = 0.0;
	}
	else
	{
		peak = peakP; age = ageP + u_phos.x; // not re-hit: hold the peak colour, advance age
	}
	gl_FragColor = vec4(peak, age);
}
