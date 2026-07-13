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
uniform vec4 u_ambient_color;   // (r,g,b,0) non-excited phosphor body tint (room-light reflection)
uniform vec4 u_ambient_level;   // (level,0,0,0) reflection floor in milli-units (x0.001), 0 = off
uniform vec4 u_ambient_mask;    // (amount,0,0,0) 0 = flat floor, 1 = floor modulated by the shadow mask

void main()
{
	vec4 base = texture2D(s_tex, v_texcoord0);

	float strength    = clamp(u_shadow_mask_strength.x,    0.0, 1.0);
	float brightboost = clamp(u_shadow_mask_brightboost.x, 0.0, 2.0);
	float slider_scale = max(1.0, u_shadow_mask_scale.x);

	// Scale proportionally to resolution against a 1920x1080 base.
	// slider_scale = N means "N pixel pitch at 1080p", keeping the on-screen relative size
	// constant at other resolutions. The resulting pitch is SNAPPED to an integer pixel count:
	// a fractional pitch (e.g. 3.56 px) beats against the pixel grid and the moire shows up as
	// coarse luma bands (9-15 px) sweeping through bright strokes - screen-fixed, so they appear
	// to crawl through moving content. Integer pitch = the mask tiles align with the pixel grid
	// and only the intended fine triad texture remains (what a real mask looks like up close).
	const float ref_width = 1920.0;
	float scale = max(1.0, floor(slider_scale * (u_target_dims.x / ref_width) + 0.5));

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

	// Ambient / non-excited phosphor body colour: room light reflected off the dark
	// phosphor raises "black" to the body tint. On a real shadow-mask CRT that reflection is modulated
	// by the mask/aperture structure, so with the lights up the mask is faintly visible in the dark.
	// Apply it here (in the mask pass) so it can carry the mask pattern. u_ambient_mask blends the floor
	// between flat (0) and fully mask-modulated (1); when the mask is off (strength 0) mask_factor is 1,
	// so the floor stays flat regardless. 0 level = off (chains without the uniforms are unaffected).
	vec3 ambient     = u_ambient_level.x * 0.001 * u_ambient_color.rgb;
	vec3 ambient_out = ambient * mix(vec3_splat(1.0), mask_factor, clamp(u_ambient_mask.x, 0.0, 1.0));

	gl_FragColor = vec4(base.rgb * mask_factor + ambient_out, base.a) * v_color0;
}
