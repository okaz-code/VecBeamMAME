$input a_position, a_color0, a_texcoord0, a_texcoord1, a_texcoord2, a_texcoord3
$output v_color0, v_texcoord1, v_texcoord0, v_texcoord2, v_texcoord3, v_texcoord4

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Vertex shader for the analytic gaussian line integral renderer.
// a_texcoord1 = (a, b, d, sigma): signed axial distances from the two endpoints,
// perpendicular distance, and the gaussian sigma (negative = point mode).
// All four interpolate linearly across the expanded quad, giving every fragment
// its exact line-local coordinates.
// a_position.z carries the per-vector intensity overrange (>=0); clip z is forced to 0 (2D ortho), so
// the slot is otherwise unused. Passed through as v_texcoord0.x for the fragment to scale the deposit.
// a_texcoord0.x carries the flat-core half-width (pixels, 0 = plain gaussian cross-section), passed
// through as v_texcoord0.y: the fragment carves a SOLID band/disc of that half-width out of the
// profile so a width-lifted beam reads as a bright band with thin edges instead of a wide blur.

#include "common.sh"

uniform vec4 u_inv_view_dims;

void main()
{
	gl_Position = mul(u_viewProj, vec4(a_position.xy, 0.0, 1.0));
#if BGFX_SHADER_LANGUAGE_HLSL && BGFX_SHADER_LANGUAGE_HLSL <= 300
	gl_Position.xy += u_inv_view_dims.xy * gl_Position.w;
#endif
	v_color0 = a_color0;
	v_texcoord1 = a_texcoord1;
	v_texcoord0 = vec2(a_position.z, a_texcoord0.x);
	// Spare vertex component carries the CPU-side Long-line classification.
	// The fragment shader writes classified Long light to a second MRT target.
	v_texcoord2 = vec2(a_texcoord0.y, 0.0);
	// Per-line endpoint-width profile: start amount, finish amount, fully-active flat-core
	// half-width, and transition distance. It remains constant across each line quad.
	v_texcoord3 = a_texcoord2;
	// Terminus dwell gain (start, finish). 1 = no boost; above 1 the fragment raises the deposit
	// near that end, weighted by the same endpoint profile the width uses.
	v_texcoord4 = a_texcoord3;
}
