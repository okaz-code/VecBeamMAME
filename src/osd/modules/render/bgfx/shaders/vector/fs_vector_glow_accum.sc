$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// One-tap narrow-glow accumulator.  The colour phosphor chain always bound the old
// the former glow-blit defocus offset to zero, making all nine texture reads identical.

#include "common.sh"

SAMPLER2D(s_tex, 0);

// Scales the post-pool aux buffers (analytic glow, halation, no-persist dots, rays) by the beam
// window's deposited fraction. The renderer used to bake this into their vertices, but their
// content only changes when a new source pass arrives, so it is built once per pass now and the
// per-present ramp arrives here instead. It has to be applied at the sample, ahead of any tonal
// reshape - scaling after a power curve is not the same scaling. 1 outside the window path.
uniform vec4 u_aux_ramp;
#define AUX_TEX2D(_sampler, _uv) (texture2D(_sampler, _uv) * u_aux_ramp.x)
uniform vec4 u_blit_intensity;

void main()
{
	gl_FragColor = AUX_TEX2D(s_tex, v_texcoord0) * u_blit_intensity.x * v_color0;
}
