$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Final colour-vector composite. The legacy HLSL CRT lens equation is applied only to the
// ambient tube face and shadow-mask coordinates. Vector emission and optical glow remain straight.

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);
SAMPLER2D(s_mask,  2);
SAMPLER2D(s_glass_scatter, 3);
SAMPLER2D(s_bezel_source, 4);
SAMPLER2D(s_bezel_length, 5);
SAMPLER2D(s_flare, 6);
SAMPLER2D(s_overlap, 7);

uniform vec4 u_defocus;
uniform vec4 u_vec_pincushion_x_quad;
uniform vec4 u_vec_pincushion_x_cubic;
uniform vec4 u_vec_pincushion_y_quad;
uniform vec4 u_vec_pincushion_y_cubic;
uniform vec4 u_glow_enable;
uniform vec4 u_masked_flare_gain;
uniform vec4 u_masked_core_peak;
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
uniform vec4 u_tube_cubic_distortion;
uniform vec4 u_tube_distort_corner;
uniform vec4 u_tube_round_corner;
uniform vec4 u_tube_smooth_border;
uniform vec4 u_tube_vignetting;
uniform vec4 u_tube_face_scale;
uniform vec4 u_vector_image_scale;
uniform vec4 u_bezel_glow_strength;
uniform vec4 u_bezel_glow_width;
uniform vec4 u_vector_render_scale;
uniform vec4 u_bezel_glow_curve;
uniform vec4 u_bezel_glow_inside;
uniform vec4 u_monitor_bezel_reflection;
uniform vec4 u_bezel_long_reflection;
uniform vec4 u_bezel_short_reflection;
uniform vec4 u_bezel_long_threshold;
uniform vec4 u_glow_tail_curve;
uniform vec4 u_glow_black_toe;
uniform vec4 u_smoked_glass_rgb;
uniform vec4 u_smoked_glass_transmission;
uniform vec4 u_glass_forward_scatter;
uniform vec4 u_glass_surface_illumination;
uniform vec4 u_mglow_amount;
uniform vec4 u_mglow_brightness;
uniform vec4 u_mglow_edge_diff;
uniform vec4 u_mglow_rgb_bands;
uniform vec4 u_mglow_rgb_band_count;
uniform vec4 u_primary_mode;
uniform vec4 u_primary_red_hue;
uniform vec4 u_primary_red_saturation;
uniform vec4 u_primary_red_brightness;
uniform vec4 u_primary_green_hue;
uniform vec4 u_primary_green_saturation;
uniform vec4 u_primary_green_brightness;
uniform vec4 u_primary_blue_hue;
uniform vec4 u_primary_blue_saturation;
uniform vec4 u_primary_blue_brightness;
uniform vec4 u_y_gain;
uniform vec4 u_chroma_a;
uniform vec4 u_chroma_b;
uniform vec4 u_chroma_c;

#define PINCUSHION_GAIN (5.0 / 30.0)
#define GLOW_BRIGHTNESS_GAIN 2.67

float tube_active_amount()
{
	float content = step(0.0001, u_shadow_mask_strength.x + u_ambient_level.x + u_mglow_amount.x);
	float geometry = abs(u_tube_distortion.x) + abs(u_tube_cubic_distortion.x)
		+ u_tube_distort_corner.x + u_tube_round_corner.x + u_tube_smooth_border.x
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

vec2 distort_centered(vec2 p, float amount, float cubic_amount)
{
	vec2 aspect = tube_aspect();
	vec2 physical = p * aspect;
	float cubic = cubic_amount > 0.0 ? cubic_amount * 1.1 : cubic_amount * 1.2;
	float r2 = dot(physical, physical);
	float f = 1.0 + r2 * (amount + cubic * sqrt(max(r2, 0.0)));

	vec2 half_extent = vec2_splat(0.5) * aspect;
	float rx2 = half_extent.x * half_extent.x;
	float ry2 = half_extent.y * half_extent.y;
	float fit_x = 1.0 + rx2 * (amount + cubic * sqrt(rx2));
	float fit_y = 1.0 + ry2 * (amount + cubic * sqrt(ry2));
	physical *= vec2(f / safe_distortion_divisor(fit_x), f / safe_distortion_divisor(fit_y));
	return physical / aspect;
}

vec2 tube_surface_uv(vec2 uv, float active)
{
	vec2 p = tube_face_coord(uv, active);
	vec2 centered = distort_centered(p, u_tube_distortion.x * active, u_tube_cubic_distortion.x * active);
	return tube_target_uv(centered);
}

vec2 tube_quad_coord(vec2 uv, float active)
{
	vec2 p = tube_face_coord(uv, active);
	float amount = u_tube_distortion.x * active;
	float cubic = u_tube_cubic_distortion.x * active;
	float corner_minimum = u_tube_distort_corner.x * active;
	float corner_extra = max(corner_minimum - (amount + cubic), 0.0);
	return distort_centered(p, amount + corner_extra, cubic);
}

float round_box(vec2 p, vec2 b, float r)
{
	vec2 q = abs(p) - b + r;
	return min(max(q.x, q.y), 0.0) + length(max(q, vec2_splat(0.0))) - r;
}

float tube_signed_distance(vec2 uv, float active)
{
	if (active < 0.5) return -1.0;
	vec2 aspect = tube_aspect();
	vec2 q = tube_quad_coord(uv, active) * aspect;
	float radius = clamp(u_tube_round_corner.x * 0.25, 0.0, 0.45);
	return round_box(q, vec2_splat(0.5) * aspect, radius);
}

float tube_face_factor(vec2 uv, float active)
{
	if (active < 0.5) return 1.0;
	float sd = tube_signed_distance(uv, active);
	vec2 dims = tube_quad_dims();
	float aa = max(fwidth(sd), 1.0 / max(min(dims.x, dims.y), 1.0));
	aa += u_tube_smooth_border.x * 0.02;
	return 1.0 - smoothstep(-aa, aa, sd);
}

float tube_vignette(vec2 uv, float active)
{
	if (active < 0.5) return 1.0;
	float amount = max(u_tube_vignetting.x, 0.0);
	vec2 aspect = tube_aspect();
	float len = length(tube_quad_coord(uv, active) * aspect) * (1.41421356 / length(aspect));
	float blur = amount * 0.75 + 0.25;
	float radius = 1.0 - amount * 0.25;
	return saturate(smoothstep(radius, radius - blur, len));
}

float bezel_glow_width_px()
{
	return max(u_bezel_glow_width.x, 1.0) * clamp(u_vector_render_scale.x, 0.1, 1.0);
}

float bezel_signed_distance(vec2 uv, float active)
{
	if (active < 0.5) return -1.0;
	vec2 aspect = tube_aspect();
	vec2 q = tube_quad_coord(uv, active) * aspect;
	float tube_radius = clamp(u_tube_round_corner.x * 0.25, 0.0, 0.45);
	// The bezel begins at the physical phosphor-face boundary, so its corner radius must be
	// identical to tube_signed_distance().  Bezel Glow Width controls only the falloff distance;
	// letting it enlarge the round-box radius classified valid corner phosphor as bezel and allowed
	// reflected light to intrude into the face.
	return round_box(q, vec2_splat(0.5) * aspect, tube_radius);
}

float bezel_band(vec2 uv, float active)
{
	if (active < 0.5 || u_ambient_level.x <= 0.0) return 0.0;
	float line_gain = max(u_bezel_glow_strength.x, 0.0);
	float monitor_gain = u_monitor_bezel_reflection.x >= 0.0 ? u_monitor_bezel_reflection.x : line_gain;
	if (line_gain <= 0.0 && monitor_gain <= 0.0) return 0.0;
	vec2 dims = tube_quad_dims();
	float signed_px = bezel_signed_distance(uv, active) * min(dims.x, dims.y);
	float width_px = bezel_glow_width_px();
	float curve = max(u_bezel_glow_curve.x, 0.25);
	float inside_balance = clamp(u_bezel_glow_inside.x, 0.0, 1.0);
	float outside = exp(-pow(max(signed_px, 0.0) / width_px, curve));
	float inside = inside_balance * exp(-pow(max(-signed_px, 0.0) / width_px, curve));
	return mix(inside, outside, step(0.0, signed_px));
}
vec2 vector_pincushion_uv(vec2 texcoord)
{
	vec2 uv = texcoord * 2.0 - 1.0;
	float x = uv.x, y = uv.y, y2 = y * y, x2 = x * x;
	float pinch_x = (u_vec_pincushion_x_quad.x + u_vec_pincushion_x_cubic.x * y2) * PINCUSHION_GAIN;
	float pinch_y = (u_vec_pincushion_y_quad.x + u_vec_pincushion_y_cubic.x * x2) * PINCUSHION_GAIN;
	x *= 1.0 + pinch_x * y2;
	y *= 1.0 + pinch_y * x2;
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

vec3 cie_color(vec3 cin)
{
	mat3 xy = mat3(u_chroma_a.xyz, u_chroma_b.xyz, u_chroma_c.xyz);
	mat3 xyz_to_srgb = mtxFromRows3(vec3(3.2406,-1.5372,-0.4986),vec3(-0.9689,1.8758,0.0415),vec3(0.0557,-0.2040,1.0570));
	vec3 cout=vec3_splat(0.0), white=vec3_splat(0.0);
	for (int i=0;i<3;++i)
	{
		float Y=u_y_gain[i], X=xy[i].x/xy[i].y*Y, Z=(1.0-xy[i].x-xy[i].y)/xy[i].y*Y;
		vec3 primary=mul(xyz_to_srgb,vec3(X,Y,Z)); cout+=primary*cin[i]; white+=primary;
	}
	return max(cout/max(white,vec3_splat(1e-4)),vec3_splat(0.0));
}

vec3 hue_rgb(float h)
{
	vec3 p = abs(fract(vec3(h, h + 0.6666667, h + 0.3333333)) * 6.0 - 3.0);
	return saturate(p - 1.0);
}

vec3 direct_primary(float base_hue, float shift_deg, float saturation, float brightness)
{
	vec3 c = hue_rgb(fract(base_hue + shift_deg / 360.0));
	float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
	return max(mix(vec3_splat(y), c, saturation), vec3_splat(0.0)) * brightness;
}

vec3 direct_color(vec3 cin)
{
	cin = max(cin, vec3_splat(0.0));
	float neutral = min(cin.r, min(cin.g, cin.b));
	vec3 chroma = cin - vec3_splat(neutral);
	vec3 pr = direct_primary(0.0, u_primary_red_hue.x, u_primary_red_saturation.x, u_primary_red_brightness.x);
	vec3 pg = direct_primary(0.3333333, u_primary_green_hue.x, u_primary_green_saturation.x, u_primary_green_brightness.x);
	vec3 pb = direct_primary(0.6666667, u_primary_blue_hue.x, u_primary_blue_saturation.x, u_primary_blue_brightness.x);
	return vec3_splat(neutral) + pr * chroma.r + pg * chroma.g + pb * chroma.b;
}

vec3 color_transform(vec3 cin)
{
	return u_primary_mode.x > 0.5 ? direct_color(cin) : cie_color(cin);
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
	vec3 long_light = min(max(texture2D(s_bezel_length, uv).rgb, vec3_splat(0.0)), light);
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
	vec3 source = apply_bezel_length_gain(best_uv, texture2D(s_bezel_source, best_uv).rgb);
	float best_peak = max(source.r, max(source.g, source.b));
	vec2 candidate_uv = edge_uv + reach_uv * 0.125;
	vec3 candidate_light = apply_bezel_length_gain(candidate_uv, texture2D(s_bezel_source, candidate_uv).rgb);
	float candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }
	candidate_uv = edge_uv + reach_uv * 0.25;
	candidate_light = apply_bezel_length_gain(candidate_uv, texture2D(s_bezel_source, candidate_uv).rgb);
	candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }
	candidate_uv = edge_uv + reach_uv * 0.5;
	candidate_light = apply_bezel_length_gain(candidate_uv, texture2D(s_bezel_source, candidate_uv).rgb);
	candidate_peak = max(candidate_light.r, max(candidate_light.g, candidate_light.b));
	if (candidate_peak > best_peak) { best_uv = candidate_uv; source = candidate_light; best_peak = candidate_peak; }
	candidate_uv = edge_uv + reach_uv;
	candidate_light = apply_bezel_length_gain(candidate_uv, texture2D(s_bezel_source, candidate_uv).rgb);
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

vec3 apply_glass_optics(vec3 c, vec3 scatter_source)
{
	// The transmission slider interpolates between the configured glass colour and clear glass.
	float transmission = clamp(u_smoked_glass_transmission.x * 0.01, 0.0, 1.0);
	float forward_scatter = clamp(u_glass_forward_scatter.x * 0.01, 0.0, 1.0);
	float surface_illumination = max(u_glass_surface_illumination.x * 0.01, 0.0);
	// Default settings are an exact identity and do not sample the glass-scatter texture.
	if (transmission >= 1.0 && forward_scatter <= 0.0 && surface_illumination <= 0.0) return c;
	vec3 glass_rgb = clamp(u_smoked_glass_rgb.rgb * 0.01, vec3_splat(0.0), vec3_splat(1.0));
	vec3 glass_filter = mix(glass_rgb, vec3_splat(1.0), transmission);
	// Forward scatter adds a transmitted halo without converting or attenuating the direct image.
	vec3 transmitted = (c + scatter_source * forward_scatter) * glass_filter;
	// Surface illumination is reflected toward the viewer and therefore bypasses bulk transmission.
	vec3 surface_lit = scatter_source * surface_illumination;
	return transmitted + surface_lit;
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
		base += color_transform(texture2D(s_flare, emit_uv).rgb)
			* (GLOW_BRIGHTNESS_GAIN * u_masked_flare_gain.x);
	// Additive vector intersections can exceed the calibrated direct-beam peak. If that overrange is
	// allowed into the HDR roll-off after masking, bright and dark mask cells converge and the slot
	// pattern appears to vanish. Limit only direct phosphor emission here, before the mask, preserving
	// hue and leaving physically scattered glow unmasked. A limit of 0 disables this correction.
	float core_limit = u_masked_core_peak.x;
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

	vec2 surface_uv = tube_surface_uv(v_texcoord0, tube_active);
	float face = tube_face_factor(v_texcoord0, tube_active);
	float vignette = tube_vignette(v_texcoord0, tube_active);
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
	float mglow_band_strength = clamp(u_mglow_rgb_bands.x, 0.0, 1.0);
	float mglow_band_count = clamp(floor(u_mglow_rgb_band_count.x + 0.5), 3.0, 24.0);
	float mglow_phase = clamp(surface_uv.x, 0.0, 1.0) * (mglow_band_count - 1.0) * 2.09439510;
	vec3 mglow_band_chroma = vec3(
		cos(mglow_phase - 2.09439510),
		cos(mglow_phase - 4.18879020),
		cos(mglow_phase));
	vec3 mglow_bands = max(vec3_splat(0.0), vec3_splat(1.0) + mglow_band_chroma * mglow_band_strength);
	mglow_bands = mix(vec3_splat(1.0), mglow_bands, face);
	vec3 monitor_out = shape_glow(vec3_splat(mintensity) * monitor_tint) * mglow_bands * face * vignette;

	vec3 glow = vec3_splat(0.0);
	bool emit_outside = emit_uv.x < 0.0 || emit_uv.x > 1.0 || emit_uv.y < 0.0 || emit_uv.y > 1.0;
	if (u_glow_enable.x > 0.0 && !emit_outside)
		glow = shape_glow(color_transform(texture2D(s_bloom, emit_uv).rgb) * GLOW_BRIGHTNESS_GAIN);
	vec2 global_delta = (v_texcoord0 - u_convergence_global.xy) * u_target_dims.xy;
	float global_sigma = max(u_convergence_global.w * 0.5 * length(u_target_dims.xy), 1.0);
	float global_weight = u_convergence_global.z * exp(-0.5 * dot(global_delta, global_delta) / (global_sigma * global_sigma));
	vec3 global_out = max(u_convergence_global_color.rgb, vec3_splat(0.0)) * global_weight * face;

	vec3 bezel = vec3_splat(0.0);
	float band = bezel_band(v_texcoord0, tube_active);
	if (band > 0.0)
	{
		float line_gain = max(u_bezel_glow_strength.x, 0.0);
		float monitor_gain = u_monitor_bezel_reflection.x >= 0.0 ? u_monitor_bezel_reflection.x : line_gain;
		vec3 edge_light = line_gain > 0.0 && u_glow_enable.x > 0.0 ? bezel_bloom_source(emit_uv) : vec3_splat(0.0);
		vec3 monitor_light = vec3_splat(mintensity) * monitor_tint;
		bezel = shape_glow(edge_light) * (band * line_gain) + shape_glow(monitor_light) * (band * monitor_gain);
	}

	// Stabilize beam-derived optical light in absolute nits when HDR Beam Peak changes. Apply this
	// after shape_glow so its nonlinear tail/toe controls do not change the compensation ratio.
	float glow_compensation = max(u_hdr_glow_compensation.x, 0.0);
	vec3 composite = (base * mask_factor + glow * glow_compensation) * face + ambient_out
		+ (monitor_out + global_out + bezel) * glow_compensation;
	float glass_activity = max(u_glass_forward_scatter.x, u_glass_surface_illumination.x);
	vec3 glass_light = vec3_splat(0.0);
	if (glass_activity > 0.0 && !emit_outside)
		glass_light = max(color_transform(texture2D(s_glass_scatter, emit_uv).rgb) * GLOW_BRIGHTNESS_GAIN, vec3_splat(0.0)) * glow_compensation;
	gl_FragColor = vec4(apply_glass_optics(composite, glass_light), 1.0) * v_color0;
}
