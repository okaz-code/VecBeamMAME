$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR present: the linear working target holds the fully composed image in absolute nits
// (vector + artwork). Encode it for the backbuffer: Rec.2020 + ST.2084 PQ for an HDR10
// swapchain (Windows/DXGI), linear extended for macOS EDR, or gamma (paper_white -> 1.0)
// for the SDR fallback.
// u_hdr_params = (beam_peak, output_reference_white, hdr_active, edr_active)

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_hdr_params;
uniform vec4 u_hdr_rolloff;      // (knee xpeak, max xpeak, saturation protect, current EDR headroom)
uniform vec4 u_sdr_rolloff;      // (knee, ceiling, shadow_curve, 0) SDR-only, paper_white units; see SDR branch below

void main()
{
	float output_reference_white = u_hdr_params.y;
	bool  hdr         = u_hdr_params.z > 0.5;
	bool  edr         = u_hdr_params.w > 0.5;   // macOS EDR: linear extended output instead of PQ

	vec3 L = max(texture2D(s_tex, v_texcoord0).rgb, vec3_splat(0.0));  // nits

	// Hue-preserving highlight roll-off. Additive crossings of bright vectors push a single primary
	// (esp. blue, the lowest-luminance panel primary) past what the display can show as that pure
	// colour, so the panel desaturates it toward white/its complement (a blue X reads purple at the
	// crossing). Cap the peak channel with a soft curve and scale ALL channels by the same factor, so
	// chromaticity (hue+saturation) is preserved - an over-bright blue stays blue, just stops getting
	// brighter, instead of turning purple. knee/max are multiples of beam_peak; max<=knee = off. A knee
	// of 1.0 leaves a single full-intensity line untouched and only rolls the brighter overlaps.
	if (hdr || edr)
	{
		float peak = max(u_hdr_params.x, 1.0);
		float m0   = max(L.r, max(L.g, L.b));   // original peak (overload indicator - additive overlap nits)
		float knee = u_hdr_rolloff.x * peak;
		float ceil = u_hdr_rolloff.y * peak;
		// CAMetalLayer does not tone-map values above current EDR headroom; they clip. Keep the user's
		// artistic ceiling, but dynamically lower it to the hardware ceiling. If available headroom
		// falls below the normal beam knee, move the knee down as well to retain a soft shoulder.
		if (edr && u_hdr_rolloff.w > 0.0)
		{
			float display_ceil = u_hdr_rolloff.w * output_reference_white;
			ceil = min(ceil, display_ceil);
			knee = min(knee, ceil * 0.85);
		}
		if (ceil > knee && m0 > knee)
		{
			// Saturated-colour protection (u_hdr_rolloff.z, 0 = off): a display renders WHITE at peak
			// by summing all three emitters, but a saturated primary above peak asks ONE emitter for
			// more light than it has - the panel clips or desaturates it (a blue line turns washed-out
			// the moment glow lands on it). Blend the roll-off ceiling from the full headroom (neutral
			// white) down to the knee (fully saturated colour): colour stays colour with its brightness
			// capped near peak, while near-white highlights keep using the HDR headroom. Grayscale
			// content has zero saturation, so monochrome chains are untouched.
			float sat = (1.0 - min(L.r, min(L.g, L.b)) / max(m0, 1e-4)) * u_hdr_rolloff.z;
			float ceil_eff = mix(ceil, knee, clamp(sat, 0.0, 1.0));
			float over   = m0 - knee;
			float range  = max(ceil_eff - knee, 1e-4);
			float rolled = knee + range * (over / (over + range));   // m0 -> inf asymptotes to ceil_eff
			L *= rolled / m0;
		}
		// (Overload whitening is done per-vector in the renderer, tied to beam_energy overdrive, NOT here
		//  from total pixel nits - so additive crossings of ordinary vectors stay their colour and only a
		//  genuinely overdriven beam blooms white. See put_analytic_line's overdrive desaturation.)
	}

	vec3 outc;
	if (edr)
	{
		// macOS EDR (extendedLinearSRGB colorspace): the compositor expects LINEAR values where
		// 1.0 == SDR reference white and values >1.0 use the display's HDR headroom. The working
		// target is in nominal/absolute nits. Numeric macOS peak calibration supplies the physical
		// EDR reference-white scale; relative auto mode supplies the nominal paper-white scale.
		// Over-bright vectors and additive crossings therefore map into the available headroom.
		// No PQ and no gamma encode: the layer colorspace applies the display transfer, so this is
		// also correct on non-EDR displays where the
		// >1.0 portion simply clips to SDR white.
		outc = L / max(output_reference_white, 1.0);
	}
	else if (hdr)
	{
		// Rec.709 -> Rec.2020 (standard).
		vec3 c2020 = max(vec3(
			dot(L, vec3(0.627402, 0.329292, 0.043306)),
			dot(L, vec3(0.069095, 0.919544, 0.011360)),
			dot(L, vec3(0.016394, 0.088028, 0.895578))), vec3_splat(0.0));
		vec3 Ln = c2020 * 0.0001;

		// ST.2084 is an OETF for each Rec.2020 component. Applying it only to max(R,G,B) preserves
		// ratios in the encoded signal, but the display applies the inverse EOTF per component and
		// therefore reconstructs the wrong linear-light chromaticity. Component-wise PQ makes the
		// decoded RGB ratios match the common linear working image and the macOS EDR path.
		vec3 Lm = pow(Ln, vec3_splat(0.1593017578125));
		outc = pow((vec3_splat(0.8359375) + 18.8515625 * Lm)
			/ (vec3_splat(1.0) + 18.6875 * Lm), vec3_splat(78.84375));
	}
	else
	{
		vec3 Ln = L / max(output_reference_white, 1.0);   // 1.0 = SDR reference white

		// SDR-only highlight shoulder: the roll-off above (u_hdr_rolloff) is anchored to beam_peak,
		// tuned for HDR's absolute-nits headroom - it typically still leaves values above paper_white
		// for SDR, which then hard-clip at the 8/10-bit UNORM swapchain the instant gl_FragColor > 1.0.
		// Same hue-preserving asymptotic formula as the beam_peak roll-off above, re-anchored to
		// paper_white units so overload/glow headroom compresses gracefully into SDR white instead of
		// clipping abruptly. u_sdr_rolloff = (knee, ceiling, shadow_curve, 0); ceiling<=knee disables
		// (same convention as u_hdr_rolloff). Default knee=ceiling=1.0 (off) so this doesn't change any
		// existing SDR calibration until deliberately tuned by eye against the HDR look.
		float sd_knee = u_sdr_rolloff.x;
		float sd_ceil = u_sdr_rolloff.y;
		float m0s = max(Ln.r, max(Ln.g, Ln.b));
		if (sd_ceil > sd_knee && m0s > sd_knee)
		{
			float over   = m0s - sd_knee;
			float range  = max(sd_ceil - sd_knee, 1e-4);
			float rolled = sd_knee + range * (over / (over + range));
			Ln *= rolled / m0s;
		}

		// Optional shadow/midtone reshape on top of the plain 1/2.2 gamma - bends the exponent, so
		// 0 and 1 stay fixed and only the path between changes (same "curve" convention used elsewhere
		// in this renderer). shadow_curve=1.0 (default) is the exact previous behaviour (pure 1/2.2).
		// >1 = darker mids (compresses near-black lift), <1 = brighter mids. Tune by eye comparing an
		// HDR/SDR toggle at fixed slider values, per vector-phosphor-overload-response-plan.md.
		//
		// Encode each component independently. The display's per-component inverse transfer then
		// reconstructs the intended linear-light RGB ratios. Magnitude-only gamma preserved code-value
		// ratios but suppressed secondary components after display decoding, exaggerating saturation
		// and making customised green/blue primaries disagree with the linear macOS EDR path.
		float sd_curve = max(u_sdr_rolloff.z, 1e-3);
		outc = pow(max(Ln, vec3_splat(0.0)), vec3_splat(sd_curve / 2.2));
	}

	gl_FragColor = vec4(outc, 1.0) * v_color0;
}
