$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// 3x3 RGB color matrix + post-mix channel gain. Spec:
//
//   1) Color Matrix (3x3, each component 0..1):
//        u_red_mix.rgb    = contribution of input R to output (R, G, B)   default (1, 0, 0)
//        u_green_mix.rgb  = contribution of input G to output (R, G, B)   default (0, 1, 0)
//        u_blue_mix.rgb   = contribution of input B to output (R, G, B)   default (0, 0, 1)
//
//      The default is the identity matrix (no color transform).
//      Examples:
//        - Setting "Red Mix Blue" to 0.5 makes in-game red lines look purplish
//          (Red input 1.0 -> output (R=1, G=0, B=0.5))
//        - Pure white (1,1,1) reflects each column sum of the matrix
//        - All values are [0..1], so subtraction is impossible - mixing only
//
//   2) Line Channel Gain (R/G/B, 0..2, default 1.0):
//        u_line_channel_gain.rgb amplifies the post-matrix intensity (e.g. blue boost).
//        Being the final post-mix gain, it is not affected by the min clamp, so gain>1 actually brightens.

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_red_mix;            // (R->R, R->G, R->B, 0) default (1,0,0,0)
uniform vec4 u_green_mix;          // (G->R, G->G, G->B, 0) default (0,1,0,0)
uniform vec4 u_blue_mix;           // (B->R, B->G, B->B, 0) default (0,0,1,0)
uniform vec4 u_line_channel_gain;  // (r, g, b, 0)        default (1,1,1,0)
// Phosphor overdrive (white-pull at overload, driven by this pixel's COMPOSITED brightness): kept for
// backward compatibility with the OLDER vector-color*/vector-monochrome*/vector-vectrex*.json chains
// that still bind these uniforms in their own "Phosphor Tint"-equivalent pass. The CURRENT
// vector-color-phosphor.json / vector-monochrome-phosphor.json chains deliberately do NOT bind these
// anymore - they moved this decision to the renderer's per-vector CORE colour computation
// (drawbgfx.cpp, put_analytic_line, gated on line_over>0) because deciding it from composited pixel
// brightness let several ordinary overlapping vectors summing past the knee at the same pixel (dense
// debris/explosions) falsely read as "overloaded" even though no single vector was - see
// vector-phosphor-overload-response-plan.md. Effect defaults below (amount=0) make this pass a no-op
// for any chain that doesn't explicitly bind these sliders.
uniform vec4 u_phosphor_overdrive;      // (amount, 0, 0, 0)  0 = off
uniform vec4 u_phosphor_overdrive_knee; // (knee, ceiling, 0, 0)
uniform vec4 u_overdrive_color;         // tint highlights saturate toward (default white 1,1,1)
uniform vec4 u_phosphor_overdrive_curve; // (curve, 0, 0, 0)  1.0 = linear

void main()
{
	vec4 c = texture2D(s_tex, v_texcoord0);

	// 3x3 color matrix
	vec3 mixed = c.r * u_red_mix.rgb
			   + c.g * u_green_mix.rgb
			   + c.b * u_blue_mix.rgb;

	// post-mix channel gain (post-saturation boost)
	mixed *= u_line_channel_gain.rgb;

	// Phosphor overdrive (legacy chains only - see the uniform comments above). amount=0 (the effect's
	// default) makes this an exact no-op, which is what every chain that doesn't bind these gets.
	float drive   = max(c.r, max(c.g, c.b));
	float knee    = u_phosphor_overdrive_knee.x;
	float ceiling = max(u_phosphor_overdrive_knee.y, knee + 1e-4);
	float t = clamp((drive - knee) / (ceiling - knee), 0.0, 1.0);
	float ocurve = max(u_phosphor_overdrive_curve.x, 1e-3);
	t = pow(t, ocurve);
	float w = t * u_phosphor_overdrive.x;
	float mag = max(max(mixed.r, mixed.g), max(mixed.b, 1e-4));
	mixed = mix(mixed, u_overdrive_color.rgb * mag, clamp(w, 0.0, 1.0));

	// No trailing 0..1 clamp: "internal" carries HDR overrange (hot dwell dots deposit multiples of
	// peak via the core - see overdrive_core) all the way to the HDR-present roll-off/PQ encode. A
	// clamp here silently flattened every overdriven core pixel to exactly 1.0x peak, making
	// overload_max/hdr_rolloff_max have no visible effect on the CORE dot's own brightness (only the
	// separately-routed glow/flare/halation/starburst content, added after this pass, showed HDR
	// headroom). mixed stays >=0 by construction (all inputs are >=0 mixes of non-negative values).

	gl_FragColor = vec4(mixed, c.a) * v_color0;
}
