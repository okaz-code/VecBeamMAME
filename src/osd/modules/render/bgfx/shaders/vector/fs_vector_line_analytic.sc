$input v_color0, v_texcoord1, v_texcoord0, v_texcoord2, v_texcoord3

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
		// Flat core: carve a SOLID band out of the cross-section. Endpoint thickness is evaluated
		// inside the true [p0,p1] stroke instead of drawing additive dots over its ends. The start/end
		// profiles taper to the ordinary body width over the requested distance; max (not sum) prevents
		// short strokes whose profiles overlap from becoming twice as wide in the middle.
		float start_round = (v_texcoord3.x < 0.0) ? 1.0 : 0.0;
		float finish_round = (v_texcoord3.y < 0.0) ? 1.0 : 0.0;
		float start_amount = (v_texcoord3.x < 0.0) ? (-v_texcoord3.x - 1.0) : v_texcoord3.x;
		float finish_amount = (v_texcoord3.y < 0.0) ? (-v_texcoord3.y - 1.0) : v_texcoord3.y;
		float body_core = max(v_texcoord0.y, 0.0);
		float end_core = max(v_texcoord3.z, 0.0);
		float transition = max(v_texcoord3.w, 1e-4);
		float end_curve = max(u_line_params.x, 0.1);
		float start_profile = start_amount * pow(clamp(1.0 - max(a, 0.0) / transition, 0.0, 1.0), end_curve);
		float finish_profile = finish_amount * pow(clamp(1.0 - max(-b, 0.0) / transition, 0.0, 1.0), end_curve);
		float end_profile = clamp(max(start_profile, finish_profile), 0.0, 1.0);
		float local_core = mix(body_core, end_core, end_profile);
		float start_radius = mix(body_core, end_core, clamp(start_amount, 0.0, 1.0));
		float finish_radius = mix(body_core, end_core, clamp(finish_amount, 0.0, 1.0));
		// Round only the enabled stroke termini, centred on the exact p0/p1 coordinates. The commanded
		// line remains the complete [p0,p1] body and each enabled semicircular LINE END extends outward
		// by its radius. This deliberately makes the visible length line_length + start_radius +
		// finish_radius, instead of consuming that radius inside the commanded line length.
		float core_sd = abs(d) - local_core;
		float start_round_zone = start_round * step(1e-4, start_radius) * (1.0 - step(start_radius, a));
		float finish_round_zone = finish_round * step(1e-4, finish_radius) * (1.0 - step(finish_radius, -b));
		// A connected endpoint must not receive a circular cap, but it must still meet the next
		// segment at full beam coverage. With MAX composition, the two 0.5 axial end roll-offs no
		// longer add to one and produced a visible dotted/gapped glyph. Give non-rounded endpoints a
		// short square support overlap instead: it suppresses the axial roll-off across the join while
		// preserving a straight (non-circular) edge. Two sigma also covers coreless Gaussian strokes.
		float start_join_support = max(start_radius, 2.0 * sg);
		float finish_join_support = max(finish_radius, 2.0 * sg);
		float start_join_zone = (1.0 - start_round) * step(-start_join_support, a) * (1.0 - step(start_join_support, a));
		float finish_join_zone = (1.0 - finish_round) * step(-finish_join_support, b) * (1.0 - step(finish_join_support, b));
		if (start_round_zone > 0.0)
			core_sd = length(vec2(a, d)) - start_radius;
		if (finish_round_zone > 0.0)
			core_sd = length(vec2(b, d)) - finish_radius;
		float dc = max(core_sd, 0.0);
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
		// Edge sharpness: reshape the perpendicular cross-section toward a flat-topped band. In a
		// rounded terminus the circular SDF is the complete beam shape, so do not multiply it by the
		// swept-line axial roll-off again; that second fade made the semicircle look pinched/pointed.
		perp = sharpen(perp, u_line_params.y);
		float endpoint_zone = clamp(max(max(start_round_zone, finish_round_zone), max(start_join_zone, finish_join_zone)), 0.0, 1.0);
		fade = perp * mix(axial, 1.0, endpoint_zone);
	}

	// Float intensity multiplier. Core/overload geometry carries z >= 0 (x1 and above); very faint
	// halation geometry may carry -1 < z < 0 so its gain is not quantized through RGBA8 vertex colour.
	// Premultiply RGB explicitly. MIN/MAX blend equations ignore blend factors on some APIs, so
	// relying on SRC_ALPHA there exposes the unattenuated expanded quad and makes every line huge.
	// The analytic effect uses ONE for RGB; under ordinary ADD this is exactly the former
	// colour.rgb * out.a contribution, while MAX now compares the correctly faded beam samples.
	float over_mult = max(0.0, 1.0 + v_texcoord0.x);
	float coverage = v_color0.a * fade * over_mult;
	vec4 deposit = vec4(v_color0.rgb * coverage, coverage);
	// v_texcoord2.x < -0.5 marks the overdrive hot core for chains that mask it as direct phosphor
	// emission. Keep it out of the ordinary unmasked glow attachment and route it through MRT 2.
	float separated_flare = step(v_texcoord2.x, -0.5);
	gl_FragData[0] = deposit * (1.0 - separated_flare);
	// The glow FBO binds a second colour attachment containing only the
	// CPU-classified Long contribution. Other views leave target 1 unbound.
	gl_FragData[1] = vec4(deposit.rgb * clamp(v_texcoord2.x, 0.0, 1.0), deposit.a) * (1.0 - separated_flare);
	gl_FragData[2] = deposit * separated_flare;
	// Overload-overlap statistics, accumulated independently of the visible core's optional MAX
	// blend. Each vector contributes a bounded h, so even an arbitrarily hot single vector has
	// effective count one. MRT 3 is RG16F in the analytic glow framebuffer.
	// Count occupied overloaded strokes, not their raw current. Star Wars deliberately changes the
	// VCTR current around explosion circles; using coverage here made the high-current quadrant reach
	// white first even though the real monitor compresses those current steps. fade preserves the
	// spatial beam profile while giving every qualifying vector the same unit peak contribution.
	float h = clamp(fade, 0.0, 1.0) * separated_flare;
	gl_FragData[3] = vec4(h, h * h, 0.0, 1.0);
}
