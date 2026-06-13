$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR working-target seed: scale the chain's normalized linear vector output to absolute
// nits (x beam_peak). Artwork/UI is then composed on top with native blend modes, and a
// final pass PQ-encodes the result.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_hdr_params;  // x = beam peak nits

void main()
{
	vec3 screen = texture2D(s_tex, v_texcoord0).rgb;
	gl_FragColor = vec4(screen * u_hdr_params.x, 1.0) * v_color0;
}
