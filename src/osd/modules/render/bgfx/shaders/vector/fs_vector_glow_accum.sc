$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// One-tap narrow-glow accumulator.  The colour phosphor chain always bound the old
// the former glow-blit defocus offset to zero, making all nine texture reads identical.

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_blit_intensity;

void main()
{
	gl_FragColor = texture2D(s_tex, v_texcoord0) * u_blit_intensity.x * v_color0;
}
