$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR PoC final encode: take the chain's (gamma-space) output, linearize, map full scale
// to u_hdr_params.x nits, convert Rec.709 -> Rec.2020 primaries and apply the ST.2084 (PQ)
// OETF for an HDR10 (RGB10A2) backbuffer. The proper implementation linearizes the whole
// chain; this PoC only proves out the bright-line headroom.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_hdr_params;  // x = peak nits mapped to full intensity (e.g. 600)

void main()
{
	vec3 srgb = texture2D(s_tex, v_texcoord0).rgb;

	// approximate sRGB EOTF
	vec3 lin = pow(max(srgb, vec3_splat(0.0)), vec3_splat(2.2));

	// Rec.709 -> Rec.2020 primaries
	vec3 c2020 = vec3(
		dot(lin, vec3(0.627402, 0.329292, 0.043306)),
		dot(lin, vec3(0.069095, 0.919544, 0.011360)),
		dot(lin, vec3(0.016394, 0.088028, 0.895578)));

	// absolute luminance normalized to the PQ 10000-nit reference
	vec3 L = c2020 * (u_hdr_params.x * 0.0001);

	// ST.2084 PQ OETF
	vec3 Lm = pow(max(L, vec3_splat(0.0)), vec3_splat(0.1593017578125));
	vec3 pq = pow((vec3_splat(0.8359375) + 18.8515625 * Lm) / (vec3_splat(1.0) + 18.6875 * Lm), vec3_splat(78.84375));

	gl_FragColor = vec4(pq, 1.0) * v_color0;
}
