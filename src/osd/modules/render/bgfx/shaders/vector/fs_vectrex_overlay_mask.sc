$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code

#include "common.sh"

SAMPLER2D(s_tex, 0);

void main()
{
	vec4 ink = texture2D(s_tex, v_texcoord0) * v_color0;
	gl_FragColor = ink;
}
