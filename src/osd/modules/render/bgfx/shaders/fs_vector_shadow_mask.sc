$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Shadow-mask shader for color vectors.
// Tile-repeats the shadow-mask texture (s_mask, e.g. aperture_1_2_bgr.png) and multiplies it into
// the input color (s_tex), emulating how a real color vector CRT excites RGB phosphors through a
// shadow mask.
//
// u_shadow_mask_strength.x:    strength    (0..1, 0 = disabled, 1 = mask fully applied)
// u_shadow_mask_brightboost.x: brightboost (0..2, compensates the mask's darkening)
// u_shadow_mask_scale.x:       scale       (mask tile pixel size, 1 = finest)
// u_target_dims.xy:            output target pixel size (set automatically by the chain system)

#include "common.sh"

SAMPLER2D(s_tex,  0);
SAMPLER2D(s_mask, 1);

uniform vec4 u_shadow_mask_strength;
uniform vec4 u_shadow_mask_brightboost;
uniform vec4 u_shadow_mask_scale;
uniform vec4 u_target_dims;

void main()
{
	vec4 base = texture2D(s_tex, v_texcoord0);

	float strength    = clamp(u_shadow_mask_strength.x,    0.0, 1.0);
	float brightboost = clamp(u_shadow_mask_brightboost.x, 0.0, 2.0);
	float slider_scale = max(1.0, u_shadow_mask_scale.x);

	// Scale proportionally to resolution against a 1920x1080 base.
	// slider_scale = N means "N pixel pitch at 1080p", keeping the on-screen relative size
	// constant at other resolutions.
	const float ref_width = 1920.0;
	float scale = slider_scale * (u_target_dims.x / ref_width);

	// convert to pixel coords, divide by scale to get the mask UV (tile repeat)
	// the sampler wrap mode is REPEAT, so UVs above 1 automatically tile
	vec2 mask_uv = v_texcoord0 * u_target_dims.xy / scale;
	vec3 mask = texture2D(s_mask, mask_uv).rgb;

	// Apply brightboost on the mask side, then interpolate by strength.
	// (The old code multiplied brightboost after the mix, so even at strength=0 the whole image
	//  brightened by 1+brightboost - that bug is fixed here.)
	//   strength=0 -> mask_factor = (1,1,1)                  -> full identity
	//   strength=1 -> mask_factor = mask * (1 + brightboost) -> compensated mask applied
	vec3 boosted_mask = mask * (1.0 + brightboost);
	vec3 mask_factor  = mix(vec3(1.0, 1.0, 1.0), boosted_mask, strength);

	gl_FragColor = vec4(base.rgb * mask_factor, base.a) * v_color0;
}
