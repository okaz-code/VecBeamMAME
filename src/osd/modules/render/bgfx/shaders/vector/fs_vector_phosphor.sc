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
SAMPLER2D(s_overlap, 2); // packed local light: rgb=density bloom, a=overload white heat

uniform vec4 u_phos;    // x = dt_ms, y = half_ms (tau), z = curve (p), w = total_ms
uniform vec4 u_phos2;   // x = energy-decay k (0 = off, uniform), y = hold_ms (no decay for this long)
uniform vec4 u_phos_rgb; // rgb = per-channel half-life multiplier (blue phosphor shorter etc.); 1,1,1 = uniform
uniform vec4 u_phos_reset; // x = meaningful fresh-hit floor; >0 makes every such hit replace old residue
uniform vec4 u_phos_composite; // x > 0.5: a weaker hit cannot darken brighter surviving phosphor
uniform vec4 u_phos_peak;  // x = direct-excitation peak limit applied before pool storage; 0 = off
uniform vec4 u_phos_radiant; // x = RGB combination brightness: 0 peak-normalised, 1 physical additive, >1 emphasis
uniform vec4 u_overlap_white_strength;
uniform vec4 u_overlap_white_brightness;

float phos_S(float age, float tau, float p, float total, float s1)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

// Two-phase energy decay for one channel: normal part (<=1) at the base half-life, overrange excess
// (>1) 'accel' times faster. Summed. Suppresses a hot deposit's extra brightness in the afterimage
// WITHOUT hollowing (the normal part stays monotonic in the stored peak, so a bright centre never
// decays below its dimmer edge). Must match fs_vector_phosphor_compose.
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

// Radiometric excitation proxy.  Do not use perceptual luma here: independent R/G/B phosphors
// contribute physical light simultaneously, so a two/three-primary hit carries more total emitted
// energy than one primary at the same drive.  Unit weights deliberately preserve the old scale for
// a full single primary; calibrated primary power weights can replace them later if measured SPDs
// become available.  Peak-channel metrics are still used for clipping, where sum-energy is wrong.
float phos_radiant_energy(vec3 rgb)
{
	rgb = max(rgb, vec3_splat(0.0));
	float peak = max(rgb.r, max(rgb.g, rgb.b));
	float additive = dot(rgb, vec3_splat(1.0));
	return peak + max(0.0, u_phos_radiant.x) * (additive - peak);
}

void main()
{
	vec3  cur   = texture2D(s_tex,  v_texcoord0).rgb;
	// Normalize the FRESH excitation before it enters temporal storage. Limiting only the final
	// composite left overlap pixels with a larger stored peak, so they remained brighter for the
	// entire decay. This makes the stored state obey the same calibrated ceiling as the visible core.
	float raw_cur_peak = max(cur.r, max(cur.g, cur.b));
	if (u_phos_peak.x > 0.0 && raw_cur_peak > u_phos_peak.x)
		cur *= u_phos_peak.x / raw_cur_peak;
	// Store the white-hot colour in the phosphor pool as well as showing its immediate direct flash.
	// This lets a dense explosion decay from white naturally, while a single overloaded vector has
	// effective count one and therefore retains its original colour throughout its afterimage.
	if (u_overlap_white_strength.x > 0.0)
	{
		float white_amount = clamp(u_overlap_white_strength.x, 0.0, 1.0)
			* clamp(texture2D(s_overlap, v_texcoord0).a, 0.0, 1.0);
		if (white_amount > 0.0)
		{
			float limited_peak = max(cur.r, max(cur.g, cur.b));
			float white_peak = limited_peak * (1.0 + max(u_overlap_white_brightness.x, 0.0) * white_amount);
			cur = mix(cur, vec3_splat(white_peak), white_amount);
		}
	}
	vec4  prev  = texture2D(s_prev, v_texcoord0);
	vec3  peakP = prev.rgb;
	float ageP  = prev.a;

	// Two-phase energy decay + per-channel (RGB) half-life. accel = overrange-excess speed-up
	// (u_phos2.x = phosphor_energy_decay); u_phos_rgb = per-channel half-life multiplier (1,1,1 =
	// uniform). The re-excite test compares total radiometric RGB excitation, not perceptual luma or
	// the brightest channel: simultaneous primaries physically add even when their perceived luma does not.
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
	// Advance the previous phosphor state to the CURRENT source-frame time before comparing it with
	// this frame's excitation. The old ordering compared against the previous frame's brighter value
	// and only added dt afterward, producing a one-frame decision lag near moving/dimming strokes.
	float advanced_age = ageP + max(u_phos.x, 0.0);
	float ageE = max(0.0, advanced_age - u_phos2.y);
	vec3 dRGB = vec3(
		phos_two(ageE, tauN.r, u_phos.z, totN.r, accel, normP.r, overP.r),
		phos_two(ageE, tauN.g, u_phos.z, totN.g, accel, normP.g, overP.g),
		phos_two(ageE, tauN.b, u_phos.z, totN.b, accel, normP.b, overP.b));
	float decayed_energy = phos_radiant_energy(dRGB);
	float cur_energy     = phos_radiant_energy(cur);

	vec3  peak;
	float age;
	// In Replace mode, a meaningful newly drawn stroke replaces the old phosphor history even when it
	// is dimmer than the surviving residue. The floor rejects the gaussian beam's near-zero tails.
	// Preserve Brighter instead uses temporal maximum composition so no fresh dark/weak sample can
	// carve a step into brighter afterglow. A zero reset floor retains brightness-only replacement.
	bool meaningful_hit = u_phos_reset.x > 0.0 && cur_energy >= u_phos_reset.x;
	bool replace_weak_hit = meaningful_hit && u_phos_composite.x < 0.5;
	if (replace_weak_hit || cur_energy >= decayed_energy)
	{
		// (Re)excited, or explicitly selected by Replace: the new excitation becomes the pixel. Do NOT adopt the
		// decayed residue here (a previous max(cur, dRGB) "fix" did): re-anchoring the residue at
		// age 0 every present combines with the Hill curve's flat shoulder (S(one present) ~ 1 at
		// long half-lives) into a residue that effectively NEVER decays - old content re-hit by new
		// content of another colour left a ghost that persisted for SECONDS, and interior pixels
		// grazed by weak deposits froze at partially-decayed levels (dark glyph interiors). The
		// clobbered-white-afterglow band this tried to cure is instead mitigated by the compose
		// pass's superposition lower bound (display >= current excitation).
		peak = cur;   age = 0.0;
	}
	else
	{
		// Temporal maximum composite: a dim gaussian end/tail cannot overwrite a brighter residue.
		// Keep both the old peak and its advanced age, so preserving it does not re-anchor/freeze it.
		peak = peakP; age = advanced_age;
	}
	gl_FragColor = vec4(peak, age);
}
