$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Colour phosphor-decay pool - EMIT pass. Reads the pool (rgb = peak colour, a = age ms) written by the
// update pass and outputs the currently displayed light = peak * S(age), where S is the same Hill
// sigmoid falloff (S(0)=1, S(half)=0.5, reaches 0 at total_ms). This is the value the bloom / tint /
// final blit consume. Kept separate from the update so the pool can keep peak+age across frames while
// the chain downstream sees an ordinary colour buffer.

#include "common.sh"

SAMPLER2D(s_tex, 0);   // pool: rgb = peak colour, a = age (ms)

uniform vec4 u_phos;   // y = half_ms (tau), z = curve (p), w = total_ms (x = dt, unused here)

float phos_S(float age, float tau, float p, float total)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	float s1 = 1.0 / (1.0 + pow(total / max(tau, 0.001), p));
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

void main()
{
	vec4 pool = texture2D(s_tex, v_texcoord0);
	gl_FragColor = vec4(pool.rgb * phos_S(pool.a, u_phos.y, u_phos.z, u_phos.w), 1.0);
}
