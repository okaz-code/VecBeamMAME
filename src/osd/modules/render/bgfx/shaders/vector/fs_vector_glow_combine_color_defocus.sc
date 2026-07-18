$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Final colour-vector composite. The legacy HLSL CRT lens equation is applied only to the
// ambient tube face and shadow-mask coordinates. Vector emission and optical glow remain straight.

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);
SAMPLER2D(s_mask,  2);

uniform vec4 u_defocus;
uniform vec4 u_vec_pincushion_x_quad;
uniform vec4 u_vec_pincushion_x_cubic;
uniform vec4 u_vec_pincushion_y_quad;
uniform vec4 u_vec_pincushion_y_cubic;
uniform vec4 u_glow_enable;
uniform vec4 u_shadow_mask_strength;
uniform vec4 u_shadow_mask_brightboost;
uniform vec4 u_shadow_mask_scale;
uniform vec4 u_target_dims;
uniform vec4 u_tex_size2;
uniform vec4 u_ambient_color;
uniform vec4 u_ambient_level;
uniform vec4 u_ambient_output_scale;
uniform vec4 u_ambient_mask;
uniform vec4 u_tube_distortion;
uniform vec4 u_tube_cubic_distortion;
uniform vec4 u_tube_distort_corner;
uniform vec4 u_tube_round_corner;
uniform vec4 u_tube_smooth_border;
uniform vec4 u_tube_vignetting;
uniform vec4 u_tube_face_scale;
uniform vec4 u_bezel_glow_strength;
uniform vec4 u_bezel_glow_width;
uniform vec4 u_bezel_glow_curve;
uniform vec4 u_bezel_glow_inside;
uniform vec4 u_glow_tail_curve;
uniform vec4 u_glow_black_toe;
uniform vec4 u_mglow_amount;
uniform vec4 u_mglow_brightness;
uniform vec4 u_mglow_edge_diff;
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
		+ u_tube_distort_corner.x + u_tube_round_corner.x + u_tube_smooth_border.x;
	return content * step(0.0001, geometry);
}

vec2 emission_uv(vec2 uv, float active)
{
	float image_scale = mix(1.0, clamp(u_tube_face_scale.x, 0.75, 1.0), active);
	return (uv - vec2_splat(0.5)) / image_scale + vec2_splat(0.5);
}

vec2 distort_centered(vec2 p, float amount, float cubic_amount)
{
	float cubic = cubic_amount > 0.0 ? cubic_amount * 1.1 : cubic_amount * 1.2;
	float r2 = dot(p, p);
	float f = 1.0 + r2 * (amount + cubic * sqrt(r2));
	f /= 1.0 + amount * 0.25 + cubic * 0.125;
	return p * f;
}

vec2 tube_surface_uv(vec2 uv, float active)
{
	vec2 p = emission_uv(uv, active) - vec2_splat(0.5);
	return distort_centered(p, u_tube_distortion.x * active, u_tube_cubic_distortion.x * active) + vec2_splat(0.5);
}

vec2 tube_quad_coord(vec2 uv, float active)
{
	vec2 p = emission_uv(uv, active) - vec2_splat(0.5);
	float corner_amount = max(u_tube_distort_corner.x, u_tube_distortion.x + u_tube_cubic_distortion.x) * active;
	return distort_centered(p, corner_amount, 0.0);
}

float round_box(vec2 p, vec2 b, float r)
{
	return length(max(abs(p) - b + r, vec2_splat(0.0))) - r;
}

float screen_signed_distance(vec2 uv, float active)
{
	float image_scale = mix(1.0, clamp(u_tube_face_scale.x, 0.75, 1.0), active);
	vec2 d = abs(uv - vec2_splat(0.5)) - vec2_splat(0.5 * image_scale);
	return min(max(d.x, d.y), 0.0) + length(max(d, vec2_splat(0.0)));
}

float tube_signed_distance(vec2 uv, float active)
{
	if (active < 0.5) return -1.0;
	vec2 q = tube_quad_coord(uv, active);
	float radius = clamp(u_tube_round_corner.x * 0.25, 0.0, 0.45);
	return round_box(q, vec2_splat(0.5), radius);
}

float tube_face_factor(vec2 uv, float active)
{
	if (active < 0.5) return 1.0;
	float sd = tube_signed_distance(uv, active);
	float aa = max(fwidth(sd), 1.0 / max(u_target_dims.x, u_target_dims.y));
	aa += u_tube_smooth_border.x * 0.02;
	return 1.0 - smoothstep(-aa, aa, sd);
}

float tube_vignette(vec2 uv, float active)
{
	if (active < 0.5) return 1.0;
	float amount = max(u_tube_vignetting.x, 0.0);
	float len = length(tube_quad_coord(uv, active));
	float blur = amount * 0.75 + 0.25;
	float radius = 1.0 - amount * 0.25;
	return saturate(smoothstep(radius, radius - blur, len));
}

float bezel_band(vec2 uv, float active)
{
	if (active < 0.5 || u_ambient_level.x <= 0.0 || u_bezel_glow_strength.x <= 0.0) return 0.0;
	float sd = screen_signed_distance(uv, active);
	float width_uv = max(u_bezel_glow_width.x / max(min(u_target_dims.x, u_target_dims.y), 1.0), 1e-5);
	float curve = max(u_bezel_glow_curve.x, 0.25);
	float outside = exp(-pow(max(sd, 0.0) / width_uv, curve));
	float inside = exp(-pow(max(-sd, 0.0) / width_uv, curve)) * clamp(u_bezel_glow_inside.x, 0.0, 1.0);
	return u_bezel_glow_strength.x * mix(inside, outside, step(0.0, sd));
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

void main()
{
	float tube_active = tube_active_amount();
	vec2 emit_uv = emission_uv(v_texcoord0, tube_active);
	vec2 base_uv = vector_pincushion_uv(emit_uv);
	bool outside = base_uv.x < 0.0 || base_uv.x > 1.0 || base_uv.y < 0.0 || base_uv.y > 1.0;
	vec3 base = outside ? vec3_splat(0.0) : sample_defocused(base_uv);

	vec2 surface_uv = tube_surface_uv(v_texcoord0, tube_active);
	float face = tube_face_factor(v_texcoord0, tube_active);
	float vignette = tube_vignette(v_texcoord0, tube_active);
	float strength = clamp(u_shadow_mask_strength.x, 0.0, 1.0);
	float brightboost = clamp(u_shadow_mask_brightboost.x, 0.0, 2.0);
	float raw_scale = max(0.25, u_shadow_mask_scale.x * (u_target_dims.x / 1920.0));
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
	vec3 monitor_out = shape_glow(vec3_splat(mintensity) * monitor_tint) * mask_factor * face * vignette;

	vec3 glow = vec3_splat(0.0);
	bool emit_outside = emit_uv.x < 0.0 || emit_uv.x > 1.0 || emit_uv.y < 0.0 || emit_uv.y > 1.0;
	if (u_glow_enable.x > 0.0 && !emit_outside)
		glow = shape_glow(color_transform(texture2D(s_bloom, emit_uv).rgb) * GLOW_BRIGHTNESS_GAIN);

	vec3 bezel = vec3_splat(0.0);
	float band = bezel_band(v_texcoord0, tube_active);
	if (band > 0.0)
	{
		vec2 edge_uv = clamp(emit_uv, vec2_splat(0.0), vec2_splat(1.0));
		vec3 edge_light = u_glow_enable.x > 0.0 ? color_transform(texture2D(s_bloom, edge_uv).rgb) * GLOW_BRIGHTNESS_GAIN : vec3_splat(0.0);
		vec3 monitor_light = vec3_splat(mintensity) * monitor_tint;
		bezel = shape_glow((edge_light + monitor_light) * band) * mix(vec3_splat(1.0), raw_mask_factor, clamp(u_ambient_mask.x, 0.0, 1.0));
	}

	gl_FragColor = vec4(base * mask_factor + ambient_out + monitor_out + glow + bezel, 1.0) * v_color0;
}