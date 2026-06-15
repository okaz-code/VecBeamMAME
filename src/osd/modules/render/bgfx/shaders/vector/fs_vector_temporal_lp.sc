$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Temporal low-pass (exponential moving average) of the final image, for flicker reduction.
// Unlike the max-persistence pass (which keeps peaks and only decays), this is a symmetric
// running average that damps ALL inter-frame variation:
//
//   out = mix(cur, prev_out, k)   ( = cur * (1 - k) + prev_out * k )
//
// k is the weight given to the running history. At steady state out converges to cur (no
// dimming), so static content is unchanged; rapid on/off variation (beam-event flicker, bloom
// shimmer) is averaged down. The cost is symmetric motion blur (moving content blends across
// frames) and a slower brightness ramp on sudden flashes - the opposite trade to the max pass,
// so the two are offered as separate knobs. k 0 = off (out = cur).

#include "common.sh"

SAMPLER2D(s_prev, 0);   // running-average history, previous frame (doublebuffer old page)
SAMPLER2D(s_tex,  1);   // current composited image

uniform vec4 u_smooth_k;   // x = history weight (0..1)

void main()
{
	vec3 cur  = texture2D(s_tex,  v_texcoord0).rgb;
	vec3 prev = texture2D(s_prev, v_texcoord0).rgb;
	gl_FragColor = vec4(mix(cur, prev, u_smooth_k.x), 1.0);
}
