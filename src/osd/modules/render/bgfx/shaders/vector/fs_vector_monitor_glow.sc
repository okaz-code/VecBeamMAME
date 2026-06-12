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

#include "common.sh"

uniform vec4 u_mglow_amount;
uniform vec4 u_mglow_brightness;
uniform vec4 u_mglow_edge_diff;

void main()
{
	vec2 d = v_texcoord0 - vec2(0.5, 0.5);
	// 0 (center) -> 1 (corners); diagonal half-length = sqrt(2)/2
	float r = clamp(length(d) * 1.41421356, 0.0, 1.0);

	// 1.0 at center, (1.0 - edge_diff) at the edge
	float brightness = mix(1.0, 1.0 - u_mglow_edge_diff.x, r);
	float intensity = u_mglow_amount.x * u_mglow_brightness.x * brightness;

	gl_FragColor = vec4(intensity, intensity, intensity, 1.0) * v_color0;
}
