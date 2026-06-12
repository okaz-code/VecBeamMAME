$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// Asymmetric pincushion distortion specific to vector CRTs.
//
//   Real vector CRTs (Wells-Gardner 19K6100 etc., used by Atari Star Wars / Asteroids / Tempest)
//   differ from raster CRTs: the deflection-yoke nonlinearity differs between X and Y, so the
//   pincushion distortion is asymmetric:
//     - horizontal lines bow inward at the top/bottom edges (= x deforms in proportion to y^2)
//     - vertical lines bow inward at the left/right edges (= y deforms in proportion to x^2)
//     - the distortion amount usually differs between X and Y
//
//   The existing hlsl/distortion (= fs_distortion.sc) is fully radially symmetric
//   (`r^2=x^2+y^2`, `xy *= f(r^2)`) and cannot produce separate X/Y pincushion amounts, so this
//   shader is added separately and inserted in series before the distortion pass.
//
//   Model (with center-relative UV in [-1, 1]):
//     pinch_x = u_vec_pincushion_x.x + u_vec_pincushion_x.y * y^2   (cubic term)
//     pinch_y = u_vec_pincushion_y.x + u_vec_pincushion_y.y * x^2
//     x *= 1 + pinch_x * y^2
//     y *= 1 + pinch_y * x^2
//
//   All defaults 0 = identity (no distortion).

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_vec_pincushion_x_quad;   // (.x = X-axis quad amount; positive = inward pincushion)
uniform vec4 u_vec_pincushion_x_cubic;  // (.x = X-axis cubic correction; emphasizes the corners)
uniform vec4 u_vec_pincushion_y_quad;   // (.x = Y-axis quad amount)
uniform vec4 u_vec_pincushion_y_cubic;  // (.x = Y-axis cubic correction)

// A UI slider value of +-1.0 is too strong as an effective value, so scale by 1/6 in-shader.
// Keeps the feel of the coarse slider (+-1.0 / step 0.01) while limiting the effective value.
#define PINCUSHION_GAIN (5.0 / 30.0)

void main()
{
	// UV (0..1) -> center-relative (-1..1)
	vec2 uv = v_texcoord0 * 2.0 - 1.0;
	float x = uv.x;
	float y = uv.y;

	float y2 = y * y;
	float x2 = x * x;

	// combine the quad + cubic terms (cubic acts as a y^4 / x^4 term) + 1/30 scale
	float pinch_x = (u_vec_pincushion_x_quad.x + u_vec_pincushion_x_cubic.x * y2) * PINCUSHION_GAIN;
	float pinch_y = (u_vec_pincushion_y_quad.x + u_vec_pincushion_y_cubic.x * x2) * PINCUSHION_GAIN;

	x *= 1.0 + pinch_x * y2;
	y *= 1.0 + pinch_y * x2;

	// back from (-1..1) to (0..1)
	vec2 distorted_uv = (vec2(x, y) + 1.0) * 0.5;

	// out of range = black (same as round_corner)
	if (distorted_uv.x < 0.0 || distorted_uv.x > 1.0 ||
		distorted_uv.y < 0.0 || distorted_uv.y > 1.0)
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	gl_FragColor = texture2D(s_tex, distorted_uv) * v_color0;
}
