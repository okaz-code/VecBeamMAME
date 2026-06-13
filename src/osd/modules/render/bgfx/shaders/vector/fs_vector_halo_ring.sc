$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Halation ring ("angel's halo"). Light from the phosphor reaches the faceplate glass, the part
// beyond the critical angle is totally-internally-reflected and lands back on the phosphor at
// radius R0, diffusing out - a ring with a sharp inner edge that a monotonic bloom PSF cannot
// make. Implemented as an annular gather of the half-res bright source: each output pixel sums
// the source around circles of radius R0 (sharp main ring) and 2*R0 (fainter second ring), plus
// a faint inner disc fill (the diffuse scatter inside the ring). Only the part of the source
// above the threshold contributes, so only the bright dwell dots (bullets) ring.
//
// u_inv_screen_dims = 1/(source texel) (half-res source), set by the chain.
// u_ring_radius.x = R0 (window px); u_ring_gain.x = ring; u_ring_gain2.x = 2nd ring;
// u_ring_fill.x = inner disc fill; u_ring_threshold.x = source threshold.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_inv_screen_dims;
uniform vec4 u_ring_radius;
uniform vec4 u_ring_gain;
uniform vec4 u_ring_gain2;
uniform vec4 u_ring_fill;
uniform vec4 u_ring_threshold;

#define RING_ANGLES 48
#define FILL_ANGLES 16

void main()
{
	// window px -> half-res source texels -> UV offset
	vec2 r1 = u_inv_screen_dims.xy * (u_ring_radius.x * 0.5);
	vec2 r2 = r1 * 2.0;
	vec3 thr = vec3_splat(u_ring_threshold.x);

	vec3 ring = vec3_splat(0.0);
	vec3 ring2 = vec3_splat(0.0);
	for (int i = 0; i < RING_ANGLES; i++)
	{
		float ang = 6.2831853071795864 * ((float(i) + 0.5) / float(RING_ANGLES));
		vec2 dir = vec2(cos(ang), sin(ang));
		// main ring sampled at two nearby radii so the angular taps merge into a smooth band
		// instead of a string of dots, with a soft inner/outer edge
		ring += max(texture2D(s_tex, v_texcoord0 + dir * (r1 * 0.95)).rgb - thr, vec3_splat(0.0));
		ring += max(texture2D(s_tex, v_texcoord0 + dir * (r1 * 1.05)).rgb - thr, vec3_splat(0.0));
		ring2 += max(texture2D(s_tex, v_texcoord0 + dir * r2).rgb - thr, vec3_splat(0.0));
	}
	ring *= 0.5;  // two radii per angle

	// inner disc: a couple of small-radius rings give the faint glow inside the halo
	vec3 fill = vec3_splat(0.0);
	for (int j = 0; j < FILL_ANGLES; j++)
	{
		float ang = 6.2831853071795864 * ((float(j) + 0.5) / float(FILL_ANGLES));
		vec2 dir = vec2(cos(ang), sin(ang));
		fill += max(texture2D(s_tex, v_texcoord0 + dir * (r1 * 0.35)).rgb - thr, vec3_splat(0.0));
		fill += max(texture2D(s_tex, v_texcoord0 + dir * (r1 * 0.65)).rgb - thr, vec3_splat(0.0));
	}

	vec3 outc = (ring  * u_ring_gain.x  + ring2 * u_ring_gain2.x) * (1.0 / float(RING_ANGLES))
			  + (fill  * u_ring_fill.x) * (0.5 / float(FILL_ANGLES));
	gl_FragColor = vec4(outc, 1.0) * v_color0;
}
