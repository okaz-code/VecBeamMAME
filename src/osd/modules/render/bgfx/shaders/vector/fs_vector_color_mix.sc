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
// Phosphor overdrive: the higher a pixel's input intensity (drive), the more its tint is pulled
// toward white (1,1,1). Reproduces the CRT look where highlights saturate to white even when the
// monochrome phosphor_color is shifted off-white.
uniform vec4 u_phosphor_overdrive;      // (amount, 0, 0, 0)  0 = off
uniform vec4 u_phosphor_overdrive_knee; // (knee,   0, 0, 0)
uniform vec4 u_overdrive_color;         // tint highlights saturate toward (default white 1,1,1; set bluish for blue-white)

void main()
{
	vec4 c = texture2D(s_tex, v_texcoord0);

	// 3x3 color matrix
	vec3 mixed = c.r * u_red_mix.rgb
			   + c.g * u_green_mix.rgb
			   + c.b * u_blue_mix.rgb;

	// post-mix channel gain (post-saturation boost)
	mixed *= u_line_channel_gain.rgb;

	// Phosphor overdrive: the more drive (= the input's grayscale luminance, including bloom) exceeds
	// the knee, the more the tint is pulled toward white. amount=0 disables it (pure tint).
	float drive = max(c.r, max(c.g, c.b));
	float knee  = u_phosphor_overdrive_knee.x;
	float w = clamp((drive - knee) / max(1e-4, 1.0 - knee), 0.0, 1.0) * u_phosphor_overdrive.x;
	mixed = mix(mixed, u_overdrive_color.rgb, clamp(w, 0.0, 1.0));

	mixed = clamp(mixed, 0.0, 1.0);

	gl_FragColor = vec4(mixed, c.a) * v_color0;
}
