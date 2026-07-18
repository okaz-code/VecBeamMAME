$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Final bloom composite pass, compatible with HLSL bloom.fx.
//   Reads base (= phosphor / post output) and bloom_sum (= weighted sum of 15 mips), computes the
//   same overdrive (cross-talk) as the HLSL original, then writes with REPLACE blend.
//
//   HLSL bloom.fx:384-391 original (overdrive):
//     float3 over = max(0, texel + bloom - 1.0) * BloomOverdrive;
//     bloom.r += (over.g + over.b) * 0.5;
//     bloom.g += (over.r + over.b) * 0.5;
//     bloom.b += (over.r + over.g) * 0.5;
//
//   HLSL bloom.fx:303-307 NoiseFactor (reproduces CRT dark-area noise shimmer):
//     return 1.0 + random * max(0.0, 0.25 * pow(E, -8 * n));
//
//   HLSL bloom.fx:396 final composite:
//     blend = texel + bloom * NoiseFactor;
//
// Since the bloom_sum target became RGBA16F the accumulation clamp is gone, and NoiseFactor is added.
//   However measurement showed "BGFX Scale 2.0 = D3D Scale 0.75" (BGFX ~2.67x weaker) remained,
//   so `BLOOM_BRIGHTNESS_GAIN` = 2.67 is applied to make UI slider values match D3D. The exact
//   cause (a D3D9/bgfx bilinear-filter difference? prepare_bloom internals?) was not fully
//   determined, but this gives UI behaviour equivalent to HLSL D3D on a measured basis.

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);

uniform vec4 u_bloom_base_weight;  // (base_weight, 0, 0, 0) equiv. to HLSL Level0Weight, default 1.0
uniform vec4 u_bloom_scale;        // (scale, 0, 0, 0) master scale of the bloom mip sum
uniform vec4 u_bloom_overdrive;    // (r, g, b, 0)
uniform vec4 u_bloom_channel_gain; // (r, g, b, 0) per-channel gain on the bloom contribution (default 1,1,1)
uniform vec4 u_time;               // (time_seconds, 0, 0, 0) auto-bound by a chain system parameter
uniform vec4 u_ambient_color;      // (r, g, b, 0) non-excited phosphor body tint (P31 grey-green / P22 grey)
uniform vec4 u_ambient_level;        // (level, 0, 0, 0) room-light reflection floor, 0 = off
uniform vec4 u_ambient_output_scale; // inverse SDR beam exposure for reflected ambient only
uniform vec4 u_bloom_noise;        // (strength, freeze, 0, 0) dark-area shimmer amount; freeze>0.5 holds the pattern (pause)

// per-pixel + per-frame hash random [0, 1], compatible with HLSL random.fx
float random(vec2 uv, float t)
{
	return fract(sin(dot(uv + vec2(t, t), vec2(12.9898, 78.233))) * 43758.5453);
}

// Measured correction factor to make UI slider values match HLSL D3D
// (BGFX Scale 2.0 ~ D3D Scale 0.75 -> ratio = 2.0/0.75 = 2.67)
#define BLOOM_BRIGHTNESS_GAIN 2.67

// Overdrive is likewise corrected: measurement shows BGFX 0.5 ~ D3D 1.0 (BGFX 2.0x weaker).
// (bloom itself is already x2.67 by BRIGHTNESS_GAIN, so the overdrive input sum_for_overdrive
//  is already aligned with base+bloom; the remaining gap is weakness on the overdrive coefficient -> x2.0)
#define BLOOM_OVERDRIVE_GAIN 2.0

void main()
{
	vec3 base  = texture2D(s_base,  v_texcoord0).rgb * u_bloom_base_weight.x;
	vec3 bloom = texture2D(s_bloom, v_texcoord0).rgb * u_bloom_scale.x * BLOOM_BRIGHTNESS_GAIN;
	// per-channel bloom gain (separate R/G/B correction, default 1,1,1)
	// corrects R/G/B appearing to bloom differently due to phosphor-efficiency differences in CRTs/monitors
	bloom *= u_bloom_channel_gain.rgb;

	// HLSL-compatible overdrive: cross-talk the amount by which base+bloom exceeds 1
	vec3 sum_for_overdrive = base + bloom;
	vec3 over = max(vec3_splat(0.0), sum_for_overdrive - vec3_splat(1.0)) * u_bloom_overdrive.rgb * BLOOM_OVERDRIVE_GAIN;
	// clamp so overdrive does not get too strong (max 1.0 per channel)
	over = min(over, vec3_splat(1.0));
	vec3 crosstalk = vec3(
		(over.g + over.b) * 0.5,
		(over.r + over.b) * 0.5,
		(over.r + over.g) * 0.5
	);
	bloom += crosstalk;

	// Dark-area shimmer (HLSL NoiseFactor): up to u_bloom_noise.x in dark areas, ~0 in bright areas.
	// Driven by u_time so it animates - but the renderer sets u_bloom_noise.y (freeze) when emulation
	// is paused (no new frame), and we then hold the pattern with a constant seed so it stops shimmering
	// while paused. u_bloom_noise.x is the strength (smaller = subtler than the old fixed 0.25).
	float seed = (u_bloom_noise.y > 0.5) ? 0.0 : u_time.x;
	float r = random(v_texcoord0, seed);
	vec3 noise_factor = vec3_splat(1.0) + r * max(vec3_splat(0.0), vec3_splat(u_bloom_noise.x) * exp(-8.0 * bloom));
	bloom *= noise_factor;

	// Ambient / non-excited phosphor body colour: room light reflected off the dark
	// phosphor and metal-back raises the "black" to the body tint. A flat additive floor; the later
	// tint / vignette / HDR-encode passes shape it through the same optics. u_ambient_level 0 = off,
	// so chains without the uniform are unaffected. The slider is expressed in milli-units
	// and scaled by 0.001 here. The floor remains a controlled visible "black" lift.
	vec3 ambient = u_ambient_level.x * 0.001 * u_ambient_color.rgb * u_ambient_output_scale.x;
	gl_FragColor = vec4(base + bloom + ambient, 1.0);
}
