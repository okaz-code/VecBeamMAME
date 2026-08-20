$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// The two mask targets are ordinary sRGB layout artwork.  CRT inputs are already linear.

#include "common.sh"

SAMPLER2D(s_screen, 0);
SAMPLER2D(s_diffused, 1);
SAMPLER2D(s_white, 2);
SAMPLER2D(s_color, 3);

uniform vec4 u_overlay_params0; // x=seed nits, y=white transmission, z=white reflectance, w=resin diffusion strength
uniform vec4 u_overlay_params1; // x=ambient, y=paper white nits, z=colour optical density, w=colour resin glow
uniform vec4 u_overlay_params2; // x=dark level, y=highlight bleach, z=highlight knee, w=highlight curve

void main()
{
	vec3 screen = texture2D(s_screen, v_texcoord0).rgb * u_overlay_params0.x;
	vec3 diffused = texture2D(s_diffused, v_texcoord0).rgb * u_overlay_params0.x;
	vec4 white_ink = texture2D(s_white, v_texcoord0);
	vec4 color_ink = texture2D(s_color, v_texcoord0);

	float white_mask = saturate(white_ink.a);
	// Light entering the transparent resin spreads laterally.  Under rear white ink, retain the
	// sharp beam but mix in the broad resin-scattered component before applying transmission.
	vec3 white_transmitted = mix(screen, diffused, u_overlay_params0.w) * u_overlay_params0.y;
	vec3 transmitted = mix(screen, white_transmitted, white_mask);

	// The scattered light is intercepted by the rear white ink and re-radiated toward the viewer.
	// Convert coloured phosphor energy to a neutral reflected-white term, matching the real plate's
	// visibly glowing white print rather than merely blurring/dimming the vector behind it.  That
	// path leaves through the resin once, like the CRT light itself.  Room light reflected by the
	// same ink is the exception: it crosses the resin on the way in as well, so it is held
	// unfiltered here and given its extra pass where the layers are summed.
	float scatter_luma = dot(diffused, vec3(0.2126, 0.7152, 0.0722));
	vec3 resin_white_glow = vec3_splat(scatter_luma * u_overlay_params0.w * white_mask);
	vec3 reflected_white = vec3_splat(u_overlay_params1.y * u_overlay_params1.x
		* u_overlay_params0.z * white_mask);

	// The role-tagged colour element is consumed here and is NOT drawn again as ordinary
	// artwork, so its layout blend mode never applies on this path; the untagged surface-print
	// element is what stays visible in ambient light.  Density decouples the optical filtering
	// from the alpha that the same element would use as a plain artwork fallback.
	//
	// The ink RGB is the resin transmission at unit optical density.  Absorption follows
	// Beer-Lambert, so transmission through density d is base^d.  The previous lerp toward
	// the ink colour saturated at exactly the ink colour once d reached 1 and could not
	// express a denser plate, and below 1 it thinned the tint linearly instead of
	// exponentially, so the control did not behave as a density.  Density is therefore no
	// longer clamped; coverage keeps the clamped value for the terms that describe how much
	// of the pixel is resin rather than how deeply it absorbs.
	float resin_density = max(0.0, color_ink.a * u_overlay_params1.z);
	float resin_coverage = saturate(resin_density);
	vec3 color_srgb = color_ink.rgb / max(color_ink.a, 0.00001);
	vec3 color_linear = pow(max(color_srgb, vec3_splat(0.0)), vec3_splat(2.2));

	// Dense CRT light visually bleaches the resin filter toward white.  Keep the unlit area dark,
	// begin the release at the configurable knee, and use a sub-linear default curve so ordinary
	// vectors already receive some release while HDR peaks approach the configured maximum.
	float source_level = saturate(max(max(screen.r, screen.g), screen.b) / max(u_overlay_params0.x, 1.0));
	float highlight_t = saturate((source_level - u_overlay_params2.z) / max(1.0 - u_overlay_params2.z, 0.0001));
	float highlight_release = u_overlay_params2.y * pow(highlight_t, u_overlay_params2.w);
	float dynamic_density = resin_density * (1.0 - highlight_release);
	// pow() needs a positive base: a channel authored as fully absorbing would otherwise
	// evaluate pow(0.0, 0.0) at zero density, which is undefined.  The floor sits 100 dB
	// below white, far under anything the display can resolve.
	vec3 absorbing = max(color_linear, vec3_splat(1.0e-5));
	vec3 color_filter = pow(absorbing, vec3_splat(dynamic_density));
	// The same transmission without the highlight release, for the terms that describe the
	// plate rather than light passing through it.  A floor that brightened wherever the beam
	// bleached the resin would not be a floor.  Kept as its own pow rather than chained off
	// color_filter so a fully released highlight cannot reach pow(0.0, 0.0).
	vec3 static_filter = pow(absorbing, vec3_splat(resin_density));

	// Only light spread beyond the sharp source reaches the resin back surface.  A sub-linear
	// response lifts the very weak scatter around ordinary vectors, while saturate caps an HDR
	// overload so it cannot look like coloured light emitted directly from the front surface.
	vec3 spread_only = max(diffused - screen, vec3_splat(0.0));
	float spread_luma = dot(spread_only, vec3(0.2126, 0.7152, 0.0722));
	float spread_normalized = spread_luma / max(u_overlay_params0.x, 1.0);
	float backscatter_response = pow(saturate(spread_normalized), 0.55);
	float backscatter_nits = u_overlay_params0.x * 0.08 * u_overlay_params1.w
		* backscatter_response * resin_coverage;
	vec3 resin_backscatter = vec3_splat(backscatter_nits);

	// Backscatter is neutral where it is generated, then traverses the coloured resin with all
	// other rear-surface light.  This keeps the phenomenon behind the plate rather than adding a
	// vivid pre-coloured emission after transmission.
	// Every rear-surface term crosses the resin once on its way out, applied below.  Ambient light
	// reflected off the white ink crossed it on the way in too, so it takes the filter twice in
	// total and gets its inbound pass here.  Without this the unlit print reads as the plate's
	// single-pass transmission, while a real plate on a white ground shows the square of it.
	vec3 behind_resin = transmitted + resin_white_glow + resin_backscatter
		+ reflected_white * color_filter;
	// Unlit resin, coloured by the plate as it actually is.  Using color_linear here coloured it
	// by the transmission at unit density however dense the plate was set, so the floor stayed
	// at single-pass brightness and washed out: at a density of six it came out about six times
	// too bright and far too desaturated, and it dominated everything else in an unlit area.
	vec3 dark_resin = static_filter * (u_overlay_params1.y * u_overlay_params2.x * resin_coverage);
	gl_FragColor = vec4(behind_resin * color_filter + dark_resin, 1.0) * v_color0;
}
