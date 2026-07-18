$input v_color0, v_texcoord1, v_texcoord0

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

// u_line_params.y = edge sharpness (super-gaussian order p). 1 = plain gaussian (unchanged); p>1
// flattens the cross-section top and steepens its shoulder, so a WIDE line reads as a sharp-edged
// bright band instead of a soft blob. Applied to the already-AA'd 0..1 profile as a pure reshape:
//   sg = exp(-(-ln g)^p)   (g^1 -> g, so p=1 is identity and the erf box-integration AA is preserved).
uniform vec4 u_line_params;

float sharpen(float g, float p)
{
	if (p <= 1.0001) return g;
	return exp(-pow(-log(max(g, 1e-6)), p));
}

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
			// rim only: a smooth gaussian band at radius R0. The inner-disc fill is drawn as a
			// separate gaussian dot (decoupled from the rim gain), not added here.
			float r = sqrt(a * a + d * d);
			fade = exp(-((r - b) * (r - b)) * inv_2s2);
		}
		else if (v_texcoord0.y > 0.001)
		{
			// flat-core point: a SOLID disc of radius v_texcoord0.y with a thin gaussian skirt (sg)
			// outside it. Width-lifted (overdriven) dots read as crisp bright discs instead of soft
			// blobs. Flat dots are large by construction, so the box-integrated sub-pixel AA of the
			// plain path below is not needed here.
			// Rounded-square dwell spot. u_line_params.w is corner roundness:
			// 0 = compact rounded square, 1 = the former circular disc. The gaussian is
			// evaluated only outside the solid SDF core, preserving the existing soft skirt.
			float half_extent = v_texcoord0.y;
			float corner = mix(half_extent * 0.15, half_extent, clamp(u_line_params.w, 0.0, 1.0));
			vec2 q = abs(vec2(a, d)) - vec2_splat(half_extent) + vec2_splat(corner);
			float sd = length(max(q, vec2_splat(0.0))) + min(max(q.x, q.y), 0.0) - corner;
			float rr = max(sd, 0.0);
			fade = sharpen(exp(-rr * rr * inv_2s2), u_line_params.y);
		}
		else
		{
			// point mode: 2D gaussian around the dwell position, box-integrated over the fragment
			// footprint on BOTH axes (separable) - the same AA as the line's perpendicular. Without it
			// a sub-pixel dot point-sampled its peak only when it happened to sit on a pixel centre, so
			// a small dot read brighter/crisper than a thin line of equal intensity (and aliased while
			// moving). Box integration conserves the dot's energy so it matches a line at the same sigma,
			// which lets points use the same thin sigma floor as lines. Normalised so peak -> 1 as w -> 0
			// (large dots unchanged). 1.2533141 = sqrt(pi/2).
			float inv_pt = 0.70710678 / sg;
			float wa = 0.5 * (abs(dFdx(a)) + abs(dFdy(a))) + 0.5 * sqrt(dFdx(a) * dFdx(a) + dFdy(a) * dFdy(a));
			float wd = 0.5 * (abs(dFdx(d)) + abs(dFdy(d))) + 0.5 * sqrt(dFdx(d) * dFdx(d) + dFdy(d) * dFdy(d));
			float fa = (wa > 1e-4) ? (sg * 1.2533141 / wa) * (erf_approx((a + 0.5 * wa) * inv_pt) - erf_approx((a - 0.5 * wa) * inv_pt)) : exp(-a * a * inv_2s2);
			float fd = (wd > 1e-4) ? (sg * 1.2533141 / wd) * (erf_approx((d + 0.5 * wd) * inv_pt) - erf_approx((d - 0.5 * wd) * inv_pt)) : exp(-d * d * inv_2s2);
			// sharpen each axis so a fat dwell dot gets a flat core + crisp rim, same as wide lines
			fade = sharpen(fa, u_line_params.y) * sharpen(fd, u_line_params.y);
		}
	}
	else
	{
		float inv_s_sqrt2 = 0.70710678 / sg;
		float axial = 0.5 * (erf_approx(a * inv_s_sqrt2) - erf_approx(b * inv_s_sqrt2));
		// Short-stroke peak normalisation (u_line_params.z blends 0..1): a stroke shorter than ~2
		// sigma never reaches the swept peak - its two erf end roll-offs overlap - so a very short
		// slow stroke read dim and soft. Dividing by the mid-stroke peak erf(len/(2 s sqrt2)) is ~1
		// for long lines (no change) and restores the full dwell-dot peak as len -> 0.
		float lpk = erf_approx((a - b) * 0.5 * inv_s_sqrt2);
		axial /= max(mix(1.0, lpk, u_line_params.z), 1e-3);
		// Flat core: carve a SOLID band of half-width v_texcoord0.y out of the cross-section - the
		// gaussian applies to the distance OUTSIDE the band (dc), so a width-lifted line is a bright
		// band with thin gaussian edges instead of one wide blur. 0 = plain gaussian (dc == |d|).
		float dc = max(abs(d) - max(v_texcoord0.y, 0.0), 0.0);
		// Perpendicular profile integrated over the fragment's footprint instead of point-sampled.
		// d is the perpendicular distance in pixels. Point sampling let H/V lines land their peak on
		// a pixel centre (crisp, fat) while diagonals fell between pixels (thin). The footprint width
		// w sets how wide the gaussian is box-averaged: at H/V both norms give 1.0, but at 45 degrees
		// Euclidean (|grad d| = 1) leaves diagonals too thin while Manhattan (|dFdx|+|dFdy| = 1.41)
		// over-blurs them fat. The midpoint of the two norms lands diagonals at ~1.21 - between the
		// two failure modes - and is exactly 1.0 at H/V (those stay put). Normalised so peak -> 1 as
		// w -> 0 (thick lines unchanged). 1.2533141 = sqrt(pi/2).
		float gx = dFdx(d), gy = dFdy(d);
		float w = 0.5 * (abs(gx) + abs(gy)) + 0.5 * sqrt(gx * gx + gy * gy);
		float perp;
		if (w > 1e-4)
		{
			float norm = sg * 1.2533141 / w;
			perp = norm * (erf_approx((dc + 0.5 * w) * inv_s_sqrt2) - erf_approx((dc - 0.5 * w) * inv_s_sqrt2));
		}
		else
		{
			perp = exp(-dc * dc * inv_2s2);
		}
		// Edge sharpness: reshape the perpendicular cross-section toward a flat-topped band (the axial
		// end roll-off is left alone so stroke ends stay round). p=1 -> unchanged.
		perp = sharpen(perp, u_line_params.y);
		fade = perp * axial;
	}

	// Float intensity multiplier. Core/overload geometry carries z >= 0 (x1 and above); very faint
	// halation geometry may carry -1 < z < 0 so its gain is not quantized through RGBA8 vertex colour.
	// The additive SRC_ALPHA blend deposits colour.rgb * out.a into the float FBO.
	float over_mult = max(0.0, 1.0 + v_texcoord0.x);
	gl_FragColor = v_color0 * vec4(1.0, 1.0, 1.0, fade * over_mult);
}
