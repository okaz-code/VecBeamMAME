$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Linear HDR chain entry: convert the (gamma-space) vector FBO into linear light.
// Everything downstream (phosphor, bloom, tail) then operates on physical energy,
// and the final pass PQ-encodes for the HDR10 backbuffer.

#include "common.sh"

SAMPLER2D(s_tex, 0);

void main()
{
	vec3 srgb = texture2D(s_tex, v_texcoord0).rgb;
	vec3 lin = pow(max(srgb, vec3_splat(0.0)), vec3_splat(2.2));
	gl_FragColor = vec4(lin, 1.0) * v_color0;
}
