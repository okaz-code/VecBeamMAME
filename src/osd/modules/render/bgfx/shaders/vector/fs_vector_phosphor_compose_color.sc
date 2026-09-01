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

// Scales the post-pool aux buffers (analytic glow, halation, no-persist dots, rays) by the beam
// window's deposited fraction. The renderer used to bake this into their vertices, but their
// content only changes when a new source pass arrives, so it is built once per pass now and the
// per-present ramp arrives here instead. It has to be applied at the sample, ahead of any tonal
// reshape - scaling after a power curve is not the same scaling. 1 outside the window path.
uniform vec4 u_aux_ramp;
#define AUX_TEX2D(_sampler, _uv) (texture2D(_sampler, _uv) * u_aux_ramp.x)

uniform vec4 u_phos;
uniform vec4 u_phos2;
                                  // z = strike flash ms (0 = off), w = flash gain
uniform vec4 u_phos_rgb;
// Fresh-excitation gain. Halation and glass scatter follow INSTANTANEOUS intensity, and the two
// sides of this max() are not alike in that respect: the spot being written is deposited in tens of
// microseconds at full beam current, while the pool residue emits its (lower) light spread over the
// whole frame. Equal frame-average values therefore do not mean equal spot brightness, and a bullet
// trail whose head sits inside phosphor_hold_ms reads exactly as bright as the dot behind it.
// 1.0 = the previous behaviour.
uniform vec4 u_fresh_gain;
uniform vec4 u_np_gain;
uniform vec4 u_line_channel_gain;
uniform vec4 u_phos_peak;
uniform vec4 u_phos_radiant;

uniform vec4 u_source_size;
uniform vec4 u_converge_red;
uniform vec4 u_converge_green;
uniform vec4 u_converge_blue;
uniform vec4 u_radial_converge_red;
uniform vec4 u_radial_converge_green;
uniform vec4 u_radial_converge_blue;

uniform vec4 u_primary_mode;
// The three colour primaries this chain resolves to, precomputed on the CPU by
// renderer_bgfx::inject_primary_basis(). Both modes used to rebuild them per pixel out of
// uniforms that cannot vary within a frame: the direct-primary helper ran three times per
// color_transform(), and color_transform() runs once here and twice in the combine pass.
// The combine pass measured ALU-bound on an Intel HD 520 (removing eight of its fourteen
// texture fetches cost it only 7%), so that was the largest piece of pure waste in it.
uniform vec4 u_primary_basis_r;
uniform vec4 u_primary_basis_g;
uniform vec4 u_primary_basis_b;

float phos_S(float age, float tau, float p, float total, float s1)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

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

// Must match the UPDATE pass: physical R/G/B emission adds by channel, whereas perceptual luma and
// max-channel peak are unsuitable for the fresh-hit diagnostic.
float phos_radiant_energy(vec3 rgb)
{
	rgb = max(rgb, vec3_splat(0.0));
	float peak = max(rgb.r, max(rgb.g, rgb.b));
	float additive = dot(rgb, vec3_splat(1.0));
	return peak + max(0.0, u_phos_radiant.x) * (additive - peak);
}

// Preserve hue while making the emitted total RGB energy match the same metric used by UPDATE.
// Strength 1 is an identity because ordinary linear RGB already adds the three primaries; 0
// normalises a mixture to its strongest primary, while values above 1 deliberately exaggerate it.
vec3 phos_combination_brightness(vec3 rgb)
{
	rgb = max(rgb, vec3_splat(0.0));
	float additive = dot(rgb, vec3_splat(1.0));
	return (additive > 1e-6) ? rgb * (phos_radiant_energy(rgb) / additive) : rgb;
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
		return AUX_TEX2D(s_np, uv).rgb;

	vec2 half_value = vec2_splat(0.5);
	vec2 inv_source = vec2_splat(1.0) / u_source_size.xy;
	vec2 uv_r = (uv - half_value) * (1.0 + u_radial_converge_red.xy) + half_value + u_converge_red.xy * inv_source;
	vec2 uv_g = (uv - half_value) * (1.0 + u_radial_converge_green.xy) + half_value + u_converge_green.xy * inv_source;
	vec2 uv_b = (uv - half_value) * (1.0 + u_radial_converge_blue.xy) + half_value + u_converge_blue.xy * inv_source;
	return vec3(AUX_TEX2D(s_np, uv_r).r, AUX_TEX2D(s_np, uv_g).g, AUX_TEX2D(s_np, uv_b).b);
}

vec3 color_transform(vec3 cin)
{
	// Direct Primary keeps the neutral axis and re-maps only the chroma standing above it;
	// the CIE path is a plain change of basis (its per-primary normalisation by the white sum
	// is componentwise, so it folds into the basis vectors). Same two results as the inlined
	// versions this replaces.
	if (u_primary_mode.x > 0.5)
	{
		cin = max(cin, vec3_splat(0.0));
		float neutral = min(cin.r, min(cin.g, cin.b));
		vec3 chroma = cin - vec3_splat(neutral);
		return vec3_splat(neutral)
			+ u_primary_basis_r.xyz * chroma.r
			+ u_primary_basis_g.xyz * chroma.g
			+ u_primary_basis_b.xyz * chroma.b;
	}
	return max(u_primary_basis_r.xyz * cin.r
		+ u_primary_basis_g.xyz * cin.g
		+ u_primary_basis_b.xyz * cin.b, vec3_splat(0.0));
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
	vec4 pool = texture2D(s_tex, v_texcoord0);
	vec3 fresh = texture2D(s_cur, v_texcoord0).rgb;
	float raw_fresh_peak = max(fresh.r, max(fresh.g, fresh.b));
	if (u_phos_peak.x > 0.0 && raw_fresh_peak > u_phos_peak.x)
		fresh *= u_phos_peak.x / raw_fresh_peak;
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

	lit = max(lit, fresh);
	// Fresh-excitation gain, applied in the pool's own scale rather than to the raw s_cur sample -
	// the two are not commensurate (measured about 5:1 on asteroid, and more before the roll-off), so
	// a gain on s_cur has to exceed that ratio before max() even notices it. age == 0 means the pixel
	// was excited THIS present, which is exactly the newest hit and nothing else.
	if (u_fresh_gain.x > 1.0 && pool.a <= 0.0)
		lit *= u_fresh_gain.x;
	lit *= phos_flash(pool.a);
	lit += sample_np_converged(v_texcoord0) * u_np_gain.x;
	vec3 composed = phos_combination_brightness(lit) * u_line_channel_gain.rgb;

	gl_FragColor = vec4(color_transform(composed), 1.0) * v_color0;
}
