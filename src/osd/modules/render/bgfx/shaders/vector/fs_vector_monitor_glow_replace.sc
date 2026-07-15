$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Monitor glow: when overloaded beams run off-screen, the whole tube face glows, brightest at the
// centre and falling off toward the edges. The amount is a CPU-accumulated value (off-screen overload
// energy gathered by the renderer from the vector device's overload-line notifier).
//
// u_mglow_amount.x:     accumulated off-screen overload energy (injected each frame by the renderer)
// u_mglow_brightness.x: Monitor Glow Brightness slider (overall multiplier)
// u_mglow_edge_diff.x:  Monitor Glow Center-Edge Diff slider (brightness drop toward the edges)
//
// IN-PLACE variant (samples s_tex and adds intensity itself, REPLACE blend) - for the current
// vector-color.json chain only, which writes this directly onto the DOUBLEBUFFERED
// "internal" target (no mglow_out/mglow_seed - lets the pass be skipped outright via
// disablewhen mglow_coefficient==0 without leaving a separate target stale). Self-sampling s_tex while
// writing the SAME logical target is only safe because "internal" is doublebuffered (read comes from
// the old page, write goes to the new one) - do NOT point this effect at a single-buffer target.
// The plain additive-blend-only fs_vector_monitor_glow.sc (this file's sibling) stays unchanged for the
// OLDER vector-color*/vector-monochrome*/vector-vectrex*.json chains, which route mglow through their
// own single-buffer "mglow_out" target seeded by a separate blit pass - additive blend hardware can
// accumulate onto that safely without a shader-side self-read, but this REPLACE+sample variant cannot.

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_mglow_amount;
uniform vec4 u_mglow_brightness;
uniform vec4 u_mglow_edge_diff;

void main()
{
	vec3 base = texture2D(s_tex, v_texcoord0).rgb;

	vec2 d = v_texcoord0 - vec2(0.5, 0.5);
	// 0 (center) -> 1 (corners); diagonal half-length = sqrt(2)/2
	float r = clamp(length(d) * 1.41421356, 0.0, 1.0);

	// 1.0 at center, (1.0 - edge_diff) at the edge
	float brightness = mix(1.0, 1.0 - u_mglow_edge_diff.x, r);
	float intensity = u_mglow_amount.x * u_mglow_brightness.x * brightness;

	gl_FragColor = vec4(base + vec3(intensity, intensity, intensity), 1.0) * v_color0;
}
