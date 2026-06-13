$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR artwork/UI shader. Draws an artwork or UI quad into the linear working target
// (absolute nits) so MAME's native blend modes compose physically over the vector
// screen: sRGB texture -> linear, modulated by the vertex colour, scaled to nits.
// u_hdr_gui.x = nits scale (paper white for opaque/alpha/add; 1.0 for multiply, whose
// source is a dimensionless transmission ratio).

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_hdr_gui;

void main()
{
	vec4 c = texture2D(s_tex, v_texcoord0);
	// The UI/artwork colour is texture x vertex colour, both in sRGB (MAME UI colours are sRGB).
	// Linearize the product: linearizing only the texture left vertex-coloured fills (e.g. the
	// menu's navy background) in gamma space, reading washed-out/pale in linear light.
	vec3 srgb = max(c.rgb * v_color0.rgb, vec3_splat(0.0));
	vec3 lin = pow(srgb, vec3_splat(2.2));
	gl_FragColor = vec4(lin * u_hdr_gui.x, c.a * v_color0.a);
}
