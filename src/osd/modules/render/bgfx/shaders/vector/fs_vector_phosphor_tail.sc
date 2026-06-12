$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Two-component phosphor decay, slow component (P31 comet tail).
// Accumulates the current line image into a float pool with a long per-frame
// retain factor:  pool = pool_prev * retain + current.
// The pool stores raw integrated energy; the composite pass scales it by the
// tail_strength slider, so strength can be tuned live without re-accumulating.
// u_tail_freeze.x is injected by the renderer: 1 while emulation time is not
// advancing (pause / menu still), which holds the pool instead of pumping it.

#include "common.sh"

SAMPLER2D(s_prev, 0);  // slow pool, previous frame (doublebuffer old page)
SAMPLER2D(s_tex, 1);   // current excitation (post-pincushion line image)

uniform vec4 u_tail_decay;   // x = per-frame retain factor (0..1)
uniform vec4 u_tail_freeze;  // x = 1 to hold the pool (no decay, no injection)

void main()
{
	vec3 prev = texture2D(s_prev, v_texcoord0).rgb;
	vec3 cur  = texture2D(s_tex,  v_texcoord0).rgb;

	float retain = mix(u_tail_decay.x, 1.0, u_tail_freeze.x);
	float inject = 1.0 - u_tail_freeze.x;

	gl_FragColor = vec4(prev * retain + cur * inject, 1.0);
}
