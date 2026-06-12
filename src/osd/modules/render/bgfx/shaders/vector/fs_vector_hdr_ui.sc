$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HDR UI composite: the UI/menu layer is rendered into an SDR offscreen with normal
// sRGB blending, then this pass maps it to paper-white nits, converts to Rec.2020 and
// PQ-encodes it for alpha-blending over the HDR10 backbuffer. Blending translucent UI
// in PQ space is slightly non-linear - accepted for the v1 composite.

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_hdr_params;  // x = paper white nits (UI white level)

void main()
{
	vec4 ui = texture2D(s_tex, v_texcoord0);

	vec3 lin = pow(max(ui.rgb, vec3_splat(0.0)), vec3_splat(2.2));

	vec3 c2020 = vec3(
		dot(lin, vec3(0.627402, 0.329292, 0.043306)),
		dot(lin, vec3(0.069095, 0.919544, 0.011360)),
		dot(lin, vec3(0.016394, 0.088028, 0.895578)));

	vec3 L = c2020 * (u_hdr_params.x * 0.0001);

	vec3 Lm = pow(max(L, vec3_splat(0.0)), vec3_splat(0.1593017578125));
	vec3 pq = pow((vec3_splat(0.8359375) + 18.8515625 * Lm) / (vec3_splat(1.0) + 18.6875 * Lm), vec3_splat(78.84375));

	gl_FragColor = vec4(pq, ui.a) * v_color0;
}
