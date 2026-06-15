$input a_position, a_color0, a_texcoord1
$output v_color0, v_texcoord1, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Vertex shader for the analytic gaussian line integral renderer.
// a_texcoord1 = (a, b, d, sigma): signed axial distances from the two endpoints,
// perpendicular distance, and the gaussian sigma (negative = point mode).
// All four interpolate linearly across the expanded quad, giving every fragment
// its exact line-local coordinates.
// a_position.z carries the per-vector intensity overrange (>=0); clip z is forced to 0 (2D ortho), so
// the slot is otherwise unused. Passed through as v_texcoord0.x for the fragment to scale the deposit.

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
	v_texcoord0 = vec2(a_position.z, 0.0);
}
