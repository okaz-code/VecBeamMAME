$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR final composite (vector-hdr-display-study.md section 4.1). Combines three layers
// physically and encodes for the HDR10 backbuffer:
//   screen : the chain output, linear light (RG11B10F)
//   under  : reflective backdrop (SDR sRGB) - additive base for the half-mirror combine
//   over   : bezel / menu / OSD (SDR sRGB, straight alpha) - covers the result
//
//   L = linearize(under) * paper_white + screen * beam_peak      [both emissive -> additive]
//   L = linearize(over) * paper_white * over.a + L * (1 - over.a) [bezel/UI occludes]
//   out = PQ(Rec2020(L / 10000))   (or gamma-encoded when the swapchain is SDR)
//
// u_hdr_params = (beam_peak_nits, paper_white_nits, hdr_active, 0)

#include "common.sh"

SAMPLER2D(s_screen, 0);  // chain output, linear
SAMPLER2D(s_under, 1);   // backdrop, sRGB
SAMPLER2D(s_over, 2);    // bezel/UI, sRGB + alpha

uniform vec4 u_hdr_params;

void main()
{
	float beam_peak  = u_hdr_params.x;
	float paper_white = u_hdr_params.y;
	bool  hdr        = u_hdr_params.z > 0.5;

	vec3 screen_lin = texture2D(s_screen, v_texcoord0).rgb;
	vec4 under = texture2D(s_under, v_texcoord0);
	vec4 over  = texture2D(s_over,  v_texcoord0);

	vec3 under_lin = pow(max(under.rgb, vec3_splat(0.0)), vec3_splat(2.2)) * paper_white;
	vec3 over_lin  = pow(max(over.rgb,  vec3_splat(0.0)), vec3_splat(2.2)) * paper_white;

	// nits in absolute terms
	vec3 L = screen_lin * beam_peak + under_lin;     // additive half-mirror combine
	L = over_lin * over.a + L * (1.0 - over.a);      // bezel / UI alpha over

	vec3 outc;
	if (hdr)
	{
		// Rec.709 -> Rec.2020, then ST.2084 PQ (normalize to the 10000-nit reference)
		vec3 c2020 = vec3(
			dot(L, vec3(0.627402, 0.329292, 0.043306)),
			dot(L, vec3(0.069095, 0.919544, 0.011360)),
			dot(L, vec3(0.016394, 0.088028, 0.895578)));
		vec3 Ln = max(c2020, vec3_splat(0.0)) * 0.0001;
		vec3 Lm = pow(Ln, vec3_splat(0.1593017578125));
		outc = pow((vec3_splat(0.8359375) + 18.8515625 * Lm) / (vec3_splat(1.0) + 18.6875 * Lm), vec3_splat(78.84375));
	}
	else
	{
		// SDR fallback: paper_white maps to 1.0, gamma encode
		outc = pow(max(L / max(paper_white, 1.0), vec3_splat(0.0)), vec3_splat(1.0 / 2.2));
	}

	gl_FragColor = vec4(outc, 1.0) * v_color0;
}
