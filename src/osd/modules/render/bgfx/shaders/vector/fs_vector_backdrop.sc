$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Half-mirror backdrop treatment (monochrome test). The backdrop artwork sits behind the half mirror,
// lit by a blacklight (UV) so fluorescent paint glows. It is combined additively with the reflected
// vector image, at a different focal plane (so it is slightly out of focus and offset = parallax).
//   - defocus: a 3x3 box blur scaled by u_bd_params.z (texels)
//   - HDR: sRGB texture -> linear, x paper_white nits (matches hdr_gui so it composes in the work target)
//   - fluorescence: saturation boost + gain + a luminance-scaled UV tint (the paint glows)
// u_bd_params  = (hdr_flag, paper_white, blur_texels, 0)
// u_bd_texel   = (1/tex_w, 1/tex_h, 0, 0)
// u_bd_fluor   = (saturation, gain, uv_amount, 0)
// u_bd_uv_color= (r, g, b, 0)   blacklight tint (blue-violet)

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_bd_params;
uniform vec4 u_bd_texel;
uniform vec4 u_bd_fluor;
uniform vec4 u_bd_uv_color;

void main()
{
	float blur = u_bd_params.z;
	vec4 c;
	if (blur > 0.001)
	{
		vec2 o = u_bd_texel.xy * blur;
		vec4 s = texture2D(s_tex, v_texcoord0) * 4.0;
		s += (texture2D(s_tex, v_texcoord0 + vec2( o.x, 0.0))
			+ texture2D(s_tex, v_texcoord0 + vec2(-o.x, 0.0))
			+ texture2D(s_tex, v_texcoord0 + vec2(0.0,  o.y))
			+ texture2D(s_tex, v_texcoord0 + vec2(0.0, -o.y))) * 2.0;
		s += texture2D(s_tex, v_texcoord0 + vec2( o.x,  o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2(-o.x,  o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2( o.x, -o.y))
		   + texture2D(s_tex, v_texcoord0 + vec2(-o.x, -o.y));
		c = s * (1.0 / 16.0);
	}
	else
	{
		c = texture2D(s_tex, v_texcoord0);
	}

	vec3 srgb = max(c.rgb * v_color0.rgb, vec3_splat(0.0));
	vec3 lin = (u_bd_params.x > 0.5) ? pow(srgb, vec3_splat(2.2)) * u_bd_params.y : srgb;

	// fluorescence under blacklight: pop the chroma, scale brightness, add a UV-tinted glow that
	// rises with the painted area's luminance (fluorescent pigments emit; bare board stays dark).
	float lum = dot(lin, vec3(0.2126, 0.7152, 0.0722));
	lin = mix(vec3_splat(lum), lin, u_bd_fluor.x);      // saturation (1 = unchanged)
	lin *= u_bd_fluor.y;                                 // gain (1 = unchanged)
	lin += u_bd_uv_color.rgb * (u_bd_fluor.z * lum);     // UV glow (0 = off)

	gl_FragColor = vec4(max(lin, vec3_splat(0.0)), c.a * v_color0.a);
}
