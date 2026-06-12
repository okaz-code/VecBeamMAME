$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Copy the input under ADD blend while multiplying by u_blit_intensity.x.
// Used to additively composite halo / bloom into a target.
//
// Port of MAME HLSL bloom.fx's Bloom Overdrive (color cross-talk).
//   Idea: a bright bloom channel "leaks" color into the others, reproducing CRT phosphor cross-talk.
//   Original (bloom.fx:384-391):
//     float3 over = max(0, texel + bloom - 1.0) * BloomOverdrive;
//     bloom.r += (over.g + over.b) * 0.5;  // leak the G/B overshoot into R
//     bloom.g += (over.r + over.b) * 0.5;
//     bloom.b += (over.r + over.g) * 0.5;
//   This mod uses BGRA8 and cannot take base+bloom-1 exactly, so it approximates by leaking
//   bloom's own over-threshold amount (max(0, bloom - 0.5)) between channels.
//   default u_bloom_overdrive = (0,0,0) disables it; (1,1,1) gives strong all-channel cross-talk.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_blit_intensity;
uniform vec4 u_bloom_overdrive;  // (r, g, b, 0) default (0,0,0,0)

void main()
{
	vec3 bloom = texture2D(s_tex, v_texcoord0).rgb;

	// Bloom Overdrive: approximate cross-talk that leaks bloom's amount above 0.5 into other channels
	// (use the vec3_splat macro since HLSL does not allow a single-argument float3(scalar) splat)
	vec3 excess = max(vec3_splat(0.0), bloom - vec3_splat(0.5)) * u_bloom_overdrive.rgb;
	vec3 crosstalk = vec3(
		(excess.g + excess.b) * 0.5,
		(excess.r + excess.b) * 0.5,
		(excess.r + excess.g) * 0.5
	);
	bloom += crosstalk;

	gl_FragColor = vec4(bloom * u_blit_intensity.x, 1.0) * v_color0;
}
