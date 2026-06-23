$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Colour phosphor-decay pool - UPDATE pass. Per pixel the pool stores rgb = peak excitation colour and
// a = age in ms since that excitation. Each present: if the fresh excitation frame (s_tex) is at least
// as bright as the decayed previous value, the pixel is (re)excited -> peak = cur, age = 0; otherwise
// the peak colour is held and age advances by dt. The displayed value (peak * S(age)) is produced by
// the companion EMIT pass. S is a sigmoid-family (Hill) falloff: S(0)=1, S(half)=0.5, normalised to
// reach 0 at total_ms. dt = 0 (paused) freezes the image. Colour-agnostic, so it works for monochrome
// (rgb equal), the Vectrex 3D imager, and Atari AVG colour vector games alike. Being purely per-pixel
// (age in the pool, not per-vector timestamps) it needs no t0/t1 - even untimed sources (AVG/DVG) work.

#include "common.sh"

SAMPLER2D(s_prev, 0);   // pool, previous frame: rgb = peak colour, a = age (ms)
SAMPLER2D(s_tex,  1);   // current fresh excitation frame (rgb)

uniform vec4 u_phos;    // x = dt_ms, y = half_ms (tau), z = curve (p), w = total_ms

float phos_S(float age, float tau, float p, float total)
{
	if (age >= total) return 0.0;
	float s  = 1.0 / (1.0 + pow(age   / max(tau, 0.001), p));
	float s1 = 1.0 / (1.0 + pow(total / max(tau, 0.001), p));   // residual at total -> subtract so it hits 0
	return clamp((s - s1) / max(1e-4, 1.0 - s1), 0.0, 1.0);
}

void main()
{
	vec3  cur   = texture2D(s_tex,  v_texcoord0).rgb;
	vec4  prev  = texture2D(s_prev, v_texcoord0);
	vec3  peakP = prev.rgb;
	float ageP  = prev.a;

	float decayed = max(max(peakP.r, peakP.g), peakP.b) * phos_S(ageP, u_phos.y, u_phos.z, u_phos.w);
	float curL    = max(max(cur.r, cur.g), cur.b);

	vec3  peak;
	float age;
	if (curL >= decayed)
	{
		peak = cur;   age = 0.0;            // (re)excited: take the new colour, restart the decay
	}
	else
	{
		peak = peakP; age = ageP + u_phos.x; // not re-hit: hold the peak colour, advance age
	}
	gl_FragColor = vec4(peak, age);
}
