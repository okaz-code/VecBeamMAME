$input v_color0, v_texcoord1

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Analytic gaussian line integral. The exposure of a gaussian spot (sigma) swept at
// constant speed along p0->p1, evaluated in closed form per fragment:
//
//   E(s,d) = exp(-d^2 / 2s^2) * 0.5 * [erf(a / (s*sqrt2)) - erf(b / (s*sqrt2))]
//     a = axial distance from p0 (signed), b = axial distance from p1 (= a - len)
//
// Mid-line the erf difference saturates at 2 -> a pure gaussian cross-profile; at the
// endpoints it rolls off through 0.5 exactly as a real swept spot does, so no separate
// geometric end caps are needed. sigma < 0 flags point mode (zero-length segment =
// dwelling beam): a plain 2D gaussian with (a, d) as the two axis offsets.

#include "common.sh"

// u_ring_params.x = halation inner-disc fill ratio (the faint glow inside the ring).
uniform vec4 u_ring_params;

// Abramowitz-Stegun 7.1.26 rational approximation, max abs error 1.5e-7
float erf_approx(float x)
{
	float s = (x < 0.0) ? -1.0 : 1.0;
	x = abs(x);
	float t = 1.0 / (1.0 + 0.3275911 * x);
	float p = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
	return s * (1.0 - p * exp(-x * x));
}

void main()
{
	float a  = v_texcoord1.x;
	float b  = v_texcoord1.y;
	float d  = v_texcoord1.z;
	float sg = abs(v_texcoord1.w);
	sg = max(sg, 1e-4);

	float inv_2s2 = 1.0 / (2.0 * sg * sg);

	float fade;
	if (v_texcoord1.w < 0.0)
	{
		if (b > 0.5)
		{
			// halation ring mode: one smooth circle centred on the dwell dot. b = ring radius R0,
			// sg = edge width. A gaussian band at radius R0 gives the raised, soft-edged rim; an
			// inner linear term fills the disc faintly (the diffuse scatter inside the halo).
			float r = sqrt(a * a + d * d);
			float band = exp(-((r - b) * (r - b)) * inv_2s2);
			// flatter (quadratic) inner-disc fill reads as a lit disc rather than a centre spike;
			// u_ring_params.x is its strength relative to the rim
			float t = clamp(r / b, 0.0, 1.0);
			float fill = (r < b) ? (1.0 - t * t) * u_ring_params.x : 0.0;
			fade = band + fill;
		}
		else
		{
			// point mode: 2D gaussian around the dwell position
			fade = exp(-(a * a + d * d) * inv_2s2);
		}
	}
	else
	{
		float inv_s_sqrt2 = 0.70710678 / sg;
		float axial = 0.5 * (erf_approx(a * inv_s_sqrt2) - erf_approx(b * inv_s_sqrt2));
		fade = exp(-d * d * inv_2s2) * axial;
	}

	gl_FragColor = v_color0 * vec4(1.0, 1.0, 1.0, fade);
}
