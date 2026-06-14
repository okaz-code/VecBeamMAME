$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR present: the linear working target holds the fully composed image in absolute nits
// (vector + artwork). Encode it for the backbuffer: Rec.2020 + ST.2084 PQ for an HDR10
// swapchain, or gamma (paper_white -> 1.0) for the SDR fallback.
// u_hdr_params = (beam_peak, paper_white, hdr_active, 0)

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_hdr_params;
uniform vec4 u_phosphor_gamut;   // (blend 0..1, 0, 0, 0) 0 = Rec.709 primaries, 1 = P22 phosphor primaries

void main()
{
	float paper_white = u_hdr_params.y;
	bool  hdr         = u_hdr_params.z > 0.5;

	vec3 L = max(texture2D(s_tex, v_texcoord0).rgb, vec3_splat(0.0));  // nits

	vec3 outc;
	if (hdr)
	{
		// Rec.709 -> Rec.2020 (standard). Blended with a game-RGB -> P22-phosphor-primaries matrix
		// (same Rec.2020 container) so phosphor_gamut pushes colours toward the real CRT phosphor
		// chromaticities - mainly a more saturated Eu red that sRGB/709 cannot reach but Rec.2020 can
		// (master plan 2-6). Both preserve the white point; blend 0 = exact current behaviour.
		vec3 c709 = vec3(
			dot(L, vec3(0.627402, 0.329292, 0.043306)),
			dot(L, vec3(0.069095, 0.919544, 0.011360)),
			dot(L, vec3(0.016394, 0.088028, 0.895578)));
		vec3 cp22 = vec3(
			dot(L, vec3( 0.626148, 0.329664, 0.044188)),
			dot(L, vec3( 0.067803, 0.920605, 0.011592)),
			dot(L, vec3(-0.001794, 0.088115, 0.913679)));
		vec3 c2020 = max(mix(c709, cp22, u_phosphor_gamut.x), vec3_splat(0.0));
		vec3 Ln = c2020 * 0.0001;
		vec3 Lm = pow(Ln, vec3_splat(0.1593017578125));
		outc = pow((vec3_splat(0.8359375) + 18.8515625 * Lm) / (vec3_splat(1.0) + 18.6875 * Lm), vec3_splat(78.84375));
	}
	else
	{
		outc = pow(L / max(paper_white, 1.0), vec3_splat(1.0 / 2.2));
	}

	gl_FragColor = vec4(outc, 1.0) * v_color0;
}
