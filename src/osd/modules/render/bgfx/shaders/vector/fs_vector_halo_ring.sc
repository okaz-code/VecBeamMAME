$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Halation ring ("angel's halo"). Light from the phosphor reaches the faceplate glass,
// the part beyond the critical angle is totally-internally-reflected and lands back on the
// phosphor at radius R0 = 2*t*tan(theta_c) from the source, diffusing back out - a ring with
// a sharp inner edge that a monotonic bloom PSF cannot make. Implemented as an annular gather:
// each output pixel sums the source around a circle of radius R0 (and a fainter 2*R0 second
// ring), so a bright point produces a ring around it. Brightest on point sources (dwell dots).
//
// u_inv_screen_dims = 1/(source texel) (half-res bloom source), set by the chain.
// u_ring_radius.x = R0 in window pixels; u_ring_gain.x = ring strength; u_ring_gain2.x = 2nd ring.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_inv_screen_dims;
uniform vec4 u_ring_radius;
uniform vec4 u_ring_gain;
uniform vec4 u_ring_gain2;
uniform vec4 u_ring_threshold;  // x = source threshold: only cores brighter than this ring (bullets)

void main()
{
	// window px -> half-res source texels -> UV offset
	vec2 r1 = u_inv_screen_dims.xy * (u_ring_radius.x * 0.5);
	vec2 r2 = r1 * 2.0;
	vec3 thr = vec3_splat(u_ring_threshold.x);

	vec3 a1 = vec3_splat(0.0);
	vec3 a2 = vec3_splat(0.0);
	const int N = 24;
	for (int i = 0; i < N; i++)
	{
		float ang = 6.2831853071795864 * (float(i) / float(N));
		vec2 dir = vec2(cos(ang), sin(ang));
		// only the part of the source above the threshold contributes, so dim lines (rocks) do not
		// ring and only the bright dwell dots (bullets, drawn brighter by the SVEC boost) do
		a1 += max(texture2D(s_tex, v_texcoord0 + dir * r1).rgb - thr, vec3_splat(0.0));
		a2 += max(texture2D(s_tex, v_texcoord0 + dir * r2).rgb - thr, vec3_splat(0.0));
	}

	vec3 ring = (a1 * u_ring_gain.x + a2 * u_ring_gain2.x) * (1.0 / float(N));
	gl_FragColor = vec4(ring, 1.0) * v_color0;
}
