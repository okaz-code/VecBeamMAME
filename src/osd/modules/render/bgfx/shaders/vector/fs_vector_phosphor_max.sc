$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Max-persistence (asymmetric phosphor) for flicker reduction. Unlike the additive leaky
// integrator in hlsl/phosphor (out = prev * k + cur, which brightens overlaps and leaves long
// trails), this rises instantly and only decays:
//
//   out = max(cur, prev * decay)
//
// A newly excited pixel jumps straight to its full brightness (sharp leading edge, no motion
// smear), while a pixel that stops being drawn fades by *decay each frame. Static content that is
// redrawn within a few frames therefore holds steady - the inter-frame flicker of beam-event mode
// is filled in - with no trail; only moving content leaves a short fading tail. decay 0 = off
// (out = cur). The pool is single-component-agnostic (per-channel max), so colours are preserved.

#include "common.sh"

SAMPLER2D(s_prev, 0);   // persistence pool, previous frame (doublebuffer old page)
SAMPLER2D(s_tex,  1);   // current line image

uniform vec4 u_persist_decay;   // x = per-frame decay/retain factor (0..1)

void main()
{
	vec3 cur  = texture2D(s_tex,  v_texcoord0).rgb;
	vec3 prev = texture2D(s_prev, v_texcoord0).rgb;
	gl_FragColor = vec4(max(cur, prev * u_persist_decay.x), 1.0);
}
