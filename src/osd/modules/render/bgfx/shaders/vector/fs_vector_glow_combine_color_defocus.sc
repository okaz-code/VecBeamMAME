$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Final colour-vector composite. The legacy HLSL CRT lens equation is applied only to the
// ambient tube face and shadow-mask coordinates. Vector emission and optical glow remain straight.

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);
SAMPLER2D(s_mask,  2);
SAMPLER2D(s_bezel_source, 3);
SAMPLER2D(s_bezel_length, 4);
SAMPLER2D(s_flare, 5);
SAMPLER2D(s_overlap, 6);

// Scales the post-pool aux buffers (analytic glow, halation, no-persist dots, rays) by the beam
// window's deposited fraction. The renderer used to bake this into their vertices, but their
// content only changes when a new source pass arrives, so it is built once per pass now and the
// per-present ramp arrives here instead. It has to be applied at the sample, ahead of any tonal
// reshape - scaling after a power curve is not the same scaling. 1 outside the window path.
uniform vec4 u_aux_ramp;
#define AUX_TEX2D(_sampler, _uv) (texture2D(_sampler, _uv) * u_aux_ramp.x)
// Bezel reflection reads the glow buffer WITHOUT u_aux_ramp - deliberately, and unlike every other
// consumer of that texture. The ramp scales whole-pass aux content down to the fraction of the sweep
// the body has deposited so far, so a halo never outshines the line it belongs to. The bezel is a
// different quantity: it is diffuse room-side light from the whole tube face, and the face is
// showing the whole pass, because the phosphor pool retains it between windowed presents. Ramping it
// made the bezel collapse at the start of every pass and recover over it - a pulse with no physical
// counterpart, and a severe one, because the ramp lands ahead of two expansive power curves
// (0.32 -> 0.16 after shape_wide_source -> 0.07 after shape_glow).
//
// s_bezel_length must be sampled the same way. long_light is min()'d against the source, so a ramped
// classification against an unramped source would clamp the long term and report ordinary long
// strokes as short.
#define BEZEL_TEX2D(_sampler, _uv) texture2D(_sampler, _uv)

uniform vec4 u_defocus;
uniform vec4 u_vec_pincushion_x_quad;
uniform vec4 u_glow_enable;
uniform vec4 u_masked_flare_gain;
uniform vec4 u_masked_core_peak;
// Direct-emission ceiling, separate from the pool-side masked_core_peak. This one exists to keep
// bright and dark shadow-mask cells apart, not to model phosphor saturation, so it is the knob that
// decides whether structure ABOVE one calibrated beam - a stroke terminus where the beam stopped -
// survives to the screen. 0 = off.
uniform vec4 u_masked_core_peak_emit;
uniform vec4 u_overlap_white_strength;
uniform vec4 u_overlap_white_brightness;
uniform vec4 u_shadow_mask_strength;
uniform vec4 u_shadow_mask_brightboost;
uniform vec4 u_shadow_mask_scale;
uniform vec4 u_target_dims;
uniform vec4 u_quad_dims;
uniform vec4 u_tex_size2;
uniform vec4 u_ambient_color;
uniform vec4 u_ambient_level;
uniform vec4 u_ambient_output_scale;
uniform vec4 u_hdr_glow_compensation;
uniform vec4 u_convergence_global;       // (centre uv x/y, gain, sigma / half-diagonal)
uniform vec4 u_convergence_global_color; // normalised linear scatter colour
uniform vec4 u_ambient_mask;
uniform vec4 u_tube_distortion;
uniform vec4 u_tube_round_corner;
uniform vec4 u_tube_vignetting;
uniform vec4 u_tube_face_scale;
uniform vec4 u_vector_image_scale;
uniform vec4 u_bezel_glow_strength;
uniform vec4 u_bezel_glow_width;
uniform vec4 u_vector_render_scale;
uniform vec4 u_bezel_glow_curve;
uniform vec4 u_monitor_bezel_reflection;
uniform vec4 u_bezel_long_reflection;
uniform vec4 u_bezel_short_reflection;
uniform vec4 u_bezel_long_threshold;
uniform vec4 u_glow_tail_curve;
uniform vec4 u_glow_black_toe;
// Same source-side reshape the wide glow pyramid applies in its first level (see
// fs_vector_downsample.sc). The bezel reflection samples the raw per-line glow attachment directly -
// it never passes through the pyramid - so without this the reshape had no effect on the reflection
// and ordinary vectors kept their full bezel halo while their wide bloom was already suppressed.
// (curve, 0, 0, 0) with curve = 1 -> identity, (pivot, 0, 0, 0).
uniform vec4 u_mglow_amount;
uniform vec4 u_mglow_brightness;
uniform vec4 u_mglow_edge_diff;
uniform vec4 u_mglow_rgb_bands;
uniform vec4 u_mglow_rgb_band_count;
uniform vec4 u_primary_mode;
// The three colour primaries this chain resolves to, precomputed on the CPU by
// renderer_bgfx::inject_primary_basis(). Both modes used to rebuild them per pixel out of
// uniforms that cannot vary within a frame: direct_primary() ran three times per
// color_transform(), and color_transform() runs twice in the combine pass and once here.
// The combine pass measured ALU-bound on an Intel HD 520 (removing eight of its fourteen
// texture fetches cost it only 7%), so that was the largest piece of pure waste in it.
uniform vec4 u_primary_basis_r;
uniform vec4 u_primary_basis_g;
uniform vec4 u_primary_basis_b;

#define PINCUSHION_GAIN (5.0 / 30.0)
#define GLOW_BRIGHTNESS_GAIN 2.67

float tube_active_amount()
{
	float content = step(0.0001, u_shadow_mask_strength.x + u_ambient_level.x + u_mglow_amount.x);
	float geometry = abs(u_tube_distortion.x) + u_tube_round_corner.x
		+ abs(1.0 - u_tube_face_scale.x);
	return content * step(0.0001, geometry);
}

vec2 emission_uv(vec2 uv)
{
	// Vector Image Scale is applied to beam coordinates before rasterisation.  Sampling the completed
	// emission texture here would clip overscan at its rectangular edge and scale the physical spot.
	return uv;
}

vec2 tube_quad_dims()
{
	return max(u_quad_dims.xy, vec2_splat(1.0));
}

vec2 tube_view_scale()
{
	return max(u_target_dims.xy, vec2_splat(1.0)) / tube_quad_dims();
}

vec2 tube_view_centered(vec2 uv)
{
	return (uv - vec2_splat(0.5)) * tube_view_scale();
}

vec2 tube_target_uv(vec2 centered)
{
	return centered / tube_view_scale() + vec2_splat(0.5);
}

vec2 tube_face_coord(vec2 uv, float active)
{
	float face_scale = mix(1.0, clamp(u_tube_face_scale.x, 0.75, 1.0), active);
	return tube_view_centered(uv) / face_scale;
}

vec2 tube_aspect()
{
	vec2 dims = tube_quad_dims();
	return dims / max(min(dims.x, dims.y), 1.0);
}

float safe_distortion_divisor(float value)
{
	return abs(value) < 1e-4 ? (value < 0.0 ? -1e-4 : 1e-4) : value;
}

vec2 distort_centered(vec2 p, float amount)
{
	vec2 aspect = tube_aspect();
	vec2 physical = p * aspect;
	float r2 = dot(physical, physical);
	float f = 1.0 + r2 * amount;

	vec2 half_extent = vec2_splat(0.5) * aspect;
	float rx2 = half_extent.x * half_extent.x;
	float ry2 = half_extent.y * half_extent.y;
	float fit_x = 1.0 + rx2 * amount;
	float fit_y = 1.0 + ry2 * amount;
	physical *= vec2(f / safe_distortion_divisor(fit_x), f / safe_distortion_divisor(fit_y));
	return physical / aspect;
}

vec2 tube_quad_coord(vec2 uv, float active)
{
	vec2 p = tube_face_coord(uv, active);
	return distort_centered(p, u_tube_distortion.x * active);
}

float round_box(vec2 p, vec2 b, float r)
{
	vec2 q = abs(p) - b + r;
	return min(max(q.x, q.y), 0.0) + length(max(q, vec2_splat(0.0))) - r;
}

// These take the distorted tube coordinate and the aspect rather than deriving them. main() needs
// the same pair for the shadow-mask surface, the face, the vignette and the bezel band, and
// deriving it inside each ran distort_centered three times per pixel for one answer.
float tube_signed_distance_at(vec2 q, vec2 aspect, float active)
{
	if (active < 0.5) return -1.0;
	float radius = clamp(u_tube_round_corner.x * 0.25, 0.0, 0.45);
	return round_box(q * aspect, vec2_splat(0.5) * aspect, radius);
}

float tube_face_factor_at(float sd, float active)
{
	if (active < 0.5) return 1.0;
	vec2 dims = tube_quad_dims();
	float aa = max(fwidth(sd), 1.0 / max(min(dims.x, dims.y), 1.0));
	return 1.0 - smoothstep(-aa, aa, sd);
}

float tube_vignette_at(vec2 q, vec2 aspect, float active)
{
	if (active < 0.5) return 1.0;
	float amount = max(u_tube_vignetting.x, 0.0);
	float len = length(q * aspect) * (1.41421356 / length(aspect));
	float blur = amount * 0.75 + 0.25;
	float radius = 1.0 - amount * 0.25;
	return saturate(smoothstep(radius, radius - blur, len));
}

float bezel_glow_width_px()
{
	return max(u_bezel_glow_width.x, 1.0) * clamp(u_vector_render_scale.x, 0.1, 1.0);
}

// The bezel begins at the physical phosphor-face boundary, so its distance is by definition the
// one tube_signed_distance_at already produced - it used to be recomputed here with a comment
// saying the two had to stay identical. Bezel Glow Width controls only the falloff distance.
float bezel_band_at(float sd, float active)
{
	if (active < 0.5 || u_ambient_level.x <= 0.0) return 0.0;
	float line_gain = max(u_bezel_glow_strength.x, 0.0);
	float monitor_gain = u_monitor_bezel_reflection.x >= 0.0 ? u_monitor_bezel_reflection.x : line_gain;
	if (line_gain <= 0.0 && monitor_gain <= 0.0) return 0.0;
	vec2 dims = tube_quad_dims();
	float signed_px = sd * min(dims.x, dims.y);
	float width_px = bezel_glow_width_px();
	float curve = max(u_bezel_glow_curve.x, 0.25);

	float outside = exp(-pow(max(signed_px, 0.0) / width_px, curve));
	return outside * step(0.0, signed_px);
}
vec2 vector_pincushion_uv(vec2 texcoord)
{
	// Zero is the identity and the transform below costs a few multiplies per pixel, so skip it.
	if (u_vec_pincushion_x_quad.x == 0.0)
		return texcoord;
	vec2 uv = texcoord * 2.0 - 1.0;
	float x = uv.x, y = uv.y, y2 = y * y;
	x *= 1.0 + u_vec_pincushion_x_quad.x * PINCUSHION_GAIN * y2;
	return (vec2(x, y) + 1.0) * 0.5;
}

vec3 sample_defocused(vec2 uv)
{
	if (u_defocus.x == 0.0 && u_defocus.y == 0.0) return texture2D(s_base, uv).rgb;
	const vec2 C1=vec2(-1.60,0.25), C2=vec2(-1.00,-0.55), C3=vec2(-0.55,1.00), C4=vec2(-0.25,-1.60);
	const vec2 C5=vec2(0.25,1.60), C6=vec2(0.55,-1.00), C7=vec2(1.00,0.55), C8=vec2(1.60,-0.25);
	vec2 d = u_defocus.xy * vec2_splat(1.0 / 1024.0);
	vec3 b = texture2D(s_base,uv).rgb + texture2D(s_base,uv+C1*d).rgb + texture2D(s_base,uv+C2*d).rgb
		+ texture2D(s_base,uv+C3*d).rgb + texture2D(s_base,uv+C4*d).rgb + texture2D(s_base,uv+C5*d).rgb
		+ texture2D(s_base,uv+C6*d).rgb + texture2D(s_base,uv+C7*d).rgb + texture2D(s_base,uv+C8*d).rgb;
	return b * (1.0 / 9.0);
}

vec3 color_transform(vec3 cin)
{
	// Direct Primary keeps the neutral axis and re-maps only the chroma standing above it;
	// the CIE path is a plain change of basis (its per-primary normalisation by the white sum
	// is componentwise, so it folds into the basis vectors). Same two results as the inlined
	// versions this replaces.
	if (u_primary_mode.x > 0.5)
	{
		cin = max(cin, vec3_splat(0.0));
		float neutral = min(cin.r, min(cin.g, cin.b));
		vec3 chroma = cin - vec3_splat(neutral);
		return vec3_splat(neutral)
			+ u_primary_basis_r.xyz * chroma.r
			+ u_primary_basis_g.xyz * chroma.g
			+ u_primary_basis_b.xyz * chroma.b;
	}
	return max(u_primary_basis_r.xyz * cin.r
		+ u_primary_basis_g.xyz * cin.g
		+ u_primary_basis_b.xyz * cin.b, vec3_splat(0.0));
}

vec3 shape_glow(vec3 c)
{
	c = max(c, vec3_splat(0.0));
	float peak = max(c.r, max(c.g, c.b));
	if (peak <= 1e-7) return vec3_splat(0.0);
	float pivot = 0.25;
	float curve = max(u_glow_tail_curve.x, 0.1);
	float shaped = peak < pivot ? pivot * pow(max(peak / pivot, 1e-6), curve) : peak;
	float toe = max(u_glow_black_toe.x, 0.0);
	if (toe > 0.0) shaped *= smoothstep(0.0, toe, peak);
	return c * (shaped / peak);
}

vec3 apply_bezel_length_gain(vec2 uv, vec3 light)
{
	// MRT attachment 1 is already classified per primitive, before additive
	// overlap. Subtraction therefore separates coincident broad glows exactly.
	vec3 long_light = min(max(BEZEL_TEX2D(s_bezel_length, uv).rgb, vec3_splat(0.0)), light);
	vec3 short_light = max(light - long_light, vec3_splat(0.0));
	return short_light * max(u_bezel_short_reflection.x, 0.0)
		+ long_light * max(u_bezel_long_reflection.x, 0.0);
}

vec2 bezel_source_min()
{
	vec2 content_scale = min(tube_quad_dims() / max(u_target_dims.xy, vec2_splat(1.0)), vec2_splat(1.0));
	return (vec2_splat(1.0) - content_scale) * 0.5;
}

vec2 bezel_source_max()
{
	return vec2_splat(1.0) - bezel_source_min();
}

vec3 bezel_bloom_axis(vec2 edge_uv, vec2 inward)
{
	vec2 source_reach_uv = vec2_splat(bezel_glow_width_px()) / max(u_target_dims.xy, vec2_splat(1.0));
	vec2 content_min = bezel_source_min();
	vec2 content_max = bezel_source_max();
	vec2 tangent_min = min(content_min + source_reach_uv, content_max);
	vec2 tangent_max = max(content_max - source_reach_uv, content_min);
	if (abs(inward.x) > 0.5)
		edge_uv.y = clamp(edge_uv.y, tangent_min.y, tangent_max.y);
	else
		edge_uv.x = clamp(edge_uv.x, tangent_min.x, tangent_max.x);
	vec2 reach_uv = inward * source_reach_uv;
	vec2 best_uv = edge_uv;
	vec3 source = apply_bezel_length_gain(best_uv, BEZEL_TEX2D(s_bezel_source, best_uv).rgb);
	float best_peak = max(source.r, max(source.g, source.b));
	vec2 candidate_uv = edge_uv + reach_uv * 0.125;
	vec3 candidate_light = apply_bezel_length_gain(candidate_uv, BEZEL_TEX2D(s_bezel_source, candidate_uv).rgb);
	float candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }
	candidate_uv = edge_uv + reach_uv * 0.25;
	candidate_light = apply_bezel_length_gain(candidate_uv, BEZEL_TEX2D(s_bezel_source, candidate_uv).rgb);
	candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }
	candidate_uv = edge_uv + reach_uv * 0.5;
	candidate_light = apply_bezel_length_gain(candidate_uv, BEZEL_TEX2D(s_bezel_source, candidate_uv).rgb);
	candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }
	candidate_uv = edge_uv + reach_uv;
	candidate_light = apply_bezel_length_gain(candidate_uv, BEZEL_TEX2D(s_bezel_source, candidate_uv).rgb);
	candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }

	return color_transform(source) * GLOW_BRIGHTNESS_GAIN;
}

vec3 bezel_bloom_source(vec2 source_uv)
{
	// Evaluate the nearest vertical and horizontal edges independently. Around
	// a corner, blend them over approximately one bezel-glow width instead of
	// switching on the exact diagonal (which produced a visible triangular seam).
	bool left = source_uv.x < 0.5;
	bool top = source_uv.y < 0.5;
	vec2 content_min = bezel_source_min();
	vec2 content_max = bezel_source_max();
	vec2 vertical_edge = vec2(left ? content_min.x : content_max.x, clamp(source_uv.y, content_min.y, content_max.y));
	vec2 horizontal_edge = vec2(clamp(source_uv.x, content_min.x, content_max.x), top ? content_min.y : content_max.y);
	float vertical_distance = min(abs(source_uv.x - content_min.x), abs(content_max.x - source_uv.x));
	float horizontal_distance = min(abs(source_uv.y - content_min.y), abs(content_max.y - source_uv.y));
	float corner_blend = bezel_glow_width_px()
		/ max(min(u_target_dims.x, u_target_dims.y), 1.0);
	float distance_delta = vertical_distance - horizontal_distance;
	if (distance_delta <= -corner_blend)
		return bezel_bloom_axis(vertical_edge, vec2(left ? 1.0 : -1.0, 0.0));
	if (distance_delta >= corner_blend)
		return bezel_bloom_axis(horizontal_edge, vec2(0.0, top ? 1.0 : -1.0));
	vec3 vertical_light = bezel_bloom_axis(vertical_edge, vec2(left ? 1.0 : -1.0, 0.0));
	vec3 horizontal_light = bezel_bloom_axis(horizontal_edge, vec2(0.0, top ? 1.0 : -1.0));
	float horizontal_weight = smoothstep(-corner_blend, corner_blend, distance_delta);
	return mix(vertical_light, horizontal_light, horizontal_weight);
}

void main()
{
	float tube_active = tube_active_amount();
	vec2 emit_uv = emission_uv(v_texcoord0);
	vec2 base_uv = vector_pincushion_uv(emit_uv);
	bool outside = base_uv.x < 0.0 || base_uv.x > 1.0 || base_uv.y < 0.0 || base_uv.y > 1.0;
	vec3 base = outside ? vec3_splat(0.0) : sample_defocused(base_uv);
	// The white-hot overdrive core is direct tube emission, not scattered halo light. It therefore
	// joins the direct phosphor image before peak limiting and shadow-mask modulation. Its historical
	// narrow-glow gain is retained; broad overload bloom remains in the unmasked glow path.
	bool flare_outside = emit_uv.x < 0.0 || emit_uv.x > 1.0 || emit_uv.y < 0.0 || emit_uv.y > 1.0;
	if (!flare_outside && u_masked_flare_gain.x > 0.0)
		base += color_transform(AUX_TEX2D(s_flare, emit_uv).rgb)
			* (GLOW_BRIGHTNESS_GAIN * u_masked_flare_gain.x);
	// Additive vector intersections can exceed the calibrated direct-beam peak. If that overrange is
	// allowed into the HDR roll-off after masking, bright and dark mask cells converge and the slot
	// pattern appears to vanish. Limit only direct phosphor emission here, before the mask, preserving
	// hue and leaving physically scattered glow unmasked. A limit of 0 disables this correction.
	float core_limit = u_masked_core_peak_emit.x;
	float core_peak = max(base.r, max(base.g, base.b));
	if (core_limit > 0.0 && core_peak > core_limit)
		base *= core_limit / core_peak;

	// A single overloaded vector keeps its source hue. Only several overloaded vectors occupying
	// the same pixel generate a white-hot direct-emission component. The overlap texture stores
	// sum(h) and sum(h^2), with every individual contribution bounded to h <= 1; their ratio gives
	// an effective contributor count and cannot mistake one extremely hot vector for many lines.
	if (!flare_outside && u_overlap_white_strength.x > 0.0)
	{
		float white_amount = clamp(u_overlap_white_strength.x, 0.0, 1.0)
			* clamp(texture2D(s_overlap, emit_uv).a, 0.0, 1.0);
		if (white_amount > 0.0)
		{
			float limited_peak = max(base.r, max(base.g, base.b));
			float white_peak = limited_peak * (1.0 + max(u_overlap_white_brightness.x, 0.0) * white_amount);
			base = mix(base, vec3_splat(white_peak), white_amount);
		}
	}

	vec2 tube_aspect_v = tube_aspect();
	vec2 tube_q = tube_quad_coord(v_texcoord0, tube_active);
	float tube_sd = tube_signed_distance_at(tube_q, tube_aspect_v, tube_active);
	vec2 surface_uv = tube_target_uv(tube_q);
	float face = tube_face_factor_at(tube_sd, tube_active);
	float vignette = tube_vignette_at(tube_q, tube_aspect_v, tube_active);
	float strength = clamp(u_shadow_mask_strength.x, 0.0, 1.0);
	float brightboost = clamp(u_shadow_mask_brightboost.x, 0.0, 2.0);
	float raw_scale = max(0.25, u_shadow_mask_scale.x * (tube_quad_dims().x / 1920.0));
	float pixel_scale = raw_scale < 1.0 ? raw_scale : floor(raw_scale + 0.5);
	vec2 mask_dims = max(u_tex_size2.xy, vec2_splat(1.0));
	vec2 mask_uv = surface_uv * u_target_dims.xy / (mask_dims * pixel_scale);
	vec3 mask = texture2D(s_mask, mask_uv).rgb;
	vec3 raw_mask_factor = mix(vec3_splat(1.0), mask * (1.0 + brightboost), strength);
	vec3 mask_factor = mix(vec3_splat(1.0), raw_mask_factor, face);

	vec3 ambient = u_ambient_level.x * 0.001 * u_ambient_color.rgb * u_ambient_output_scale.x;
	vec3 ambient_out = ambient * mix(vec3_splat(1.0), mask_factor, clamp(u_ambient_mask.x, 0.0, 1.0)) * face * vignette;

	vec2 md = v_texcoord0 - vec2_splat(0.5);
	float mr = clamp(length(md) * 1.41421356, 0.0, 1.0);
	float mbright = mix(1.0, 1.0 - u_mglow_edge_diff.x, mr);
	float mintensity = u_mglow_amount.x * u_mglow_brightness.x * mbright;
	float tint_peak = max(max(u_ambient_color.r, u_ambient_color.g), max(u_ambient_color.b, 1e-4));
	vec3 monitor_tint = u_ambient_color.rgb / tint_peak;
	// The observed real monitor has only a few broad, full-height colour bands across the complete
	// face (approximately B-R-G-B-R-G-B-R), rather than a magnified phosphor-cell texture. Generate
	// that low-frequency colour-purity/interference pattern directly in curved tube-surface space.
	// Constant surface_uv.x contours bend with the same glass distortion as the slot mask while each
	// band remains vertically uniform in tube space. Three 120-degree cosine phases give the repeating
	// B/R/G order with smooth optical transitions. Their sum is zero, preserving average brightness.
	// Three cosines for a result that is exactly 1 when the strength is zero, and the branch is on
	// a uniform so it stays coherent across the draw.
	float mglow_band_strength = clamp(u_mglow_rgb_bands.x, 0.0, 1.0);
	vec3 mglow_bands = vec3_splat(1.0);
	if (mglow_band_strength > 0.0)
	{
		float mglow_band_count = clamp(floor(u_mglow_rgb_band_count.x + 0.5), 3.0, 24.0);
		float mglow_phase = clamp(surface_uv.x, 0.0, 1.0) * (mglow_band_count - 1.0) * 2.09439510;
		vec3 mglow_band_chroma = vec3(
			cos(mglow_phase - 2.09439510),
			cos(mglow_phase - 4.18879020),
			cos(mglow_phase));
		mglow_bands = max(vec3_splat(0.0), vec3_splat(1.0) + mglow_band_chroma * mglow_band_strength);
		mglow_bands = mix(vec3_splat(1.0), mglow_bands, face);
	}
	// The bezel branch below needs the same shaped monitor light, and used to call shape_glow - a
	// pow - a second time on an identical argument.
	vec3 monitor_light = vec3_splat(mintensity) * monitor_tint;
	vec3 shaped_monitor = shape_glow(monitor_light);
	vec3 monitor_out = shaped_monitor * mglow_bands * face * vignette;

	vec3 glow = vec3_splat(0.0);
	bool emit_outside = emit_uv.x < 0.0 || emit_uv.x > 1.0 || emit_uv.y < 0.0 || emit_uv.y > 1.0;
	if (u_glow_enable.x > 0.0 && !emit_outside)
		glow = shape_glow(color_transform(texture2D(s_bloom, emit_uv).rgb) * GLOW_BRIGHTNESS_GAIN);
	// Gain is a uniform, so this branch is coherent; at zero the exp and the sqrt inside length()
	// produced nothing.
	float global_weight = 0.0;
	if (u_convergence_global.z > 0.0)
	{
		vec2 global_delta = (v_texcoord0 - u_convergence_global.xy) * u_target_dims.xy;
		float global_sigma = max(u_convergence_global.w * 0.5 * length(u_target_dims.xy), 1.0);
		global_weight = u_convergence_global.z * exp(-0.5 * dot(global_delta, global_delta) / (global_sigma * global_sigma));
	}
	vec3 global_out = max(u_convergence_global_color.rgb, vec3_splat(0.0)) * global_weight * face;

	vec3 bezel = vec3_splat(0.0);
	float band = bezel_band_at(tube_sd, tube_active);
	if (band > 0.0)
	{
		float line_gain = max(u_bezel_glow_strength.x, 0.0);
		float monitor_gain = u_monitor_bezel_reflection.x >= 0.0 ? u_monitor_bezel_reflection.x : line_gain;
		// NOT shape_wide_source. That reshape was added to this branch to imitate the wide glow
		// pyramid's per-tap curve, on the reasoning that the bezel samples the raw glow buffer
		// instead of the pyramid's output. But it lands on top of shape_glow, which the reflection
		// already goes through, and on THIS chain both are expansive (glow_wide_curve 1.6,
		// glow_tail_curve 1.44), so they multiplied each other. Measured on a Space Duel bonus stage,
		// whose box is a long stroke a few pixels inside the tube edge - the ideal case for this
		// effect - the light arriving at the bezel is about 0.035 and the pair took it to 0.0009: an
		// 18x cut on top of shape_glow's own, which no setting of the strength knob could pay back.
		// The monochrome shader never had the wide reshape, which is why the same effect works there.
		vec3 edge_light = line_gain > 0.0 && u_glow_enable.x > 0.0
			? bezel_bloom_source(emit_uv) : vec3_splat(0.0);
		// shape_glow does not belong here either: a reflection is linear in the light that falls on
		// it, and with glow_tail_curve 1.44 that curve costs another 2.2x at this level. Restoring
		// only the pre-regression form (shape_glow kept) measured 1/255 at the top of the strength
		// range on the Space Duel box - still not a reflection. bezel_glow_strength is now a plain
		// reflectance against a source that is the SCATTER buffer (analytic_glow 0.12), not the
		// emitted image, so the useful range is well above 1; hence the wider cap.
		bezel = edge_light * (band * line_gain) + shaped_monitor * (band * monitor_gain);
	}

	// Stabilize beam-derived optical light in absolute nits when HDR Beam Peak changes. Apply this
	// after shape_glow so its nonlinear tail/toe controls do not change the compensation ratio.
	float glow_compensation = max(u_hdr_glow_compensation.x, 0.0);
	vec3 composite = (base * mask_factor + glow * glow_compensation) * face + ambient_out
		+ (monitor_out + global_out + bezel) * glow_compensation;
	gl_FragColor = vec4(composite, 1.0) * v_color0;
}
