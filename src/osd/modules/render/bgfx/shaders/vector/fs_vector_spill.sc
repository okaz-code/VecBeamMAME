$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Saturation "spill" (ported from VIDE's spillShader.fs).
//
// Where the beam is brighter than a threshold, the over-threshold energy bleeds into the
// surrounding pixels: orthogonal neighbours get 15% of their excess, diagonals 10%. This makes
// highlights grow in SHAPE (not just turn white like the overdrive tint does) the way a real CRT
// blooms when the phosphor saturates locally. Unlike the global bloom ladder this is a fixed 1-pixel
// neighbourhood, so it is independent of total scene light.
//
// HDR/energy-additive: each output pixel keeps its own value and ADDS the spill received from its
// neighbours (so it only brightens/widens, never darkens). u_spill_strength.x = 0 -> identity (off),
// which is the default, so existing tuning is unchanged until the slider is raised.
//
// u_inv_screen_dims is set automatically by the chain system to (1/w, 1/h) of the input texture.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_inv_screen_dims;   // (1/w, 1/h, 0, 0) auto
uniform vec4 u_spill_threshold;   // (threshold, 0, 0, 0)
uniform vec4 u_spill_strength;    // (strength,  0, 0, 0)  0 = off

void main()
{
	vec2  o = u_inv_screen_dims.xy;
	float t = u_spill_threshold.x;
	float k = u_spill_strength.x;

	vec3 c = texture2D(s_tex, v_texcoord0).rgb;

	// over-threshold excess received from the 8 neighbours (per channel, so it stays colour-correct
	// for the future 3D-imager / colour-vector chains).
	vec3 add = vec3_splat(0.0);
	// orthogonal neighbours: 15%
	add += max(texture2D(s_tex, v_texcoord0 + vec2(-o.x, 0.0)).rgb - t, 0.0) * 0.15;
	add += max(texture2D(s_tex, v_texcoord0 + vec2( o.x, 0.0)).rgb - t, 0.0) * 0.15;
	add += max(texture2D(s_tex, v_texcoord0 + vec2( 0.0,-o.y)).rgb - t, 0.0) * 0.15;
	add += max(texture2D(s_tex, v_texcoord0 + vec2( 0.0, o.y)).rgb - t, 0.0) * 0.15;
	// diagonal neighbours: 10%
	add += max(texture2D(s_tex, v_texcoord0 + vec2(-o.x,-o.y)).rgb - t, 0.0) * 0.10;
	add += max(texture2D(s_tex, v_texcoord0 + vec2( o.x,-o.y)).rgb - t, 0.0) * 0.10;
	add += max(texture2D(s_tex, v_texcoord0 + vec2(-o.x, o.y)).rgb - t, 0.0) * 0.10;
	add += max(texture2D(s_tex, v_texcoord0 + vec2( o.x, o.y)).rgb - t, 0.0) * 0.10;

	c += add * k;

	gl_FragColor = vec4(c, 1.0) * v_color0;
}
