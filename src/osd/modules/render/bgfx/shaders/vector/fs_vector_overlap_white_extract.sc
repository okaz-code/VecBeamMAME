$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Convert additive overload occupancy statistics into a direct-white heat field.

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_overlap_white_count;

void main()
{
	vec4 raw_stats = max(texture2D(s_tex, v_texcoord0), vec4_splat(0.0));
	vec2 stats = raw_stats.rg;
	float sum_h = stats.x;
	float effective_count = sum_h * sum_h / max(stats.y, 1e-5);
	float count_start = max(u_overlap_white_count.x, 1.0);
	float count_gate = smoothstep(count_start, count_start + 2.0, effective_count);
	// Reject isolated gaussian tails even when several broad strokes happen to graze the pixel.
	float energy_start = max(0.75, count_start * 0.45);
	float energy_full = max(energy_start + 0.5, count_start * 0.85);
	float white_heat = count_gate * smoothstep(energy_start, energy_full, sum_h);

	gl_FragColor = vec4(0.0, 0.0, 0.0, white_heat) * v_color0;
}
