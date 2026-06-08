$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Analytic-AA fragment shader for vector lines, with overload (defocus) support.
//   v_texcoord0.x = per-line overload value [0..1] (put_solid_line stores the overload amount).
//   v_texcoord0.y = position across the line width [0..1], 0.5 = center.
// The parabolic fade 1 - (2|y-0.5|)^2 gives a smooth, angle-independent width profile because
// the GPU interpolates the coordinate linearly across the quad. As the overload value rises the
// profile blends toward a wider Gaussian, defocusing the beam the way an overdriven CRT does.

#include "common.sh"

// u_line_params.x: Overload Softness multiplier (slider). Higher = the beam defocuses sooner/more
// as it overloads (the Gaussian widens at a lower overload value).
uniform vec4 u_line_params;

void main()
{
	float d = 2.0 * abs(v_texcoord0.y - 0.5);  // 0 (center) -> 1 (edge)
	float ovld = v_texcoord0.x;                // per-line overload [0..1]

	// normal beam: sharp parabola (1.0 at center, 0.0 at edge)
	float fade_sharp = max(0.0, 1.0 - d * d);

	// overloaded beam: Gaussian falloff (large k = tight, small k = soft/wide)
	float k = mix(8.0, 2.0, clamp(ovld * u_line_params.x, 0.0, 1.0));
	float fade_soft = exp(-d * d * k);

	// blend: overload 0 -> sharp, overload 1 -> soft (defocused)
	float fade = mix(fade_sharp, fade_soft, ovld);

	gl_FragColor = v_color0 * vec4(1.0, 1.0, 1.0, fade);
}
