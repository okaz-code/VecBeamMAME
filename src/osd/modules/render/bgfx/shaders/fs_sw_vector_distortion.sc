$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// ベクター CRT 独自の非対称糸巻き歪み (Pincushion)
//
//   実機ベクター CRT (Wells-Gardner 19K6100 等、Atari Star Wars / Asteroids /
//   Tempest 等で使用) はラスター CRT と異なり deflection yoke の非線形性が
//   X/Y で別になっており、糸巻き歪みも非対称に出る:
//     - 水平線は上下端で内側にへこむ (= y² に比例して x が変形)
//     - 垂直線は左右端で内側にへこむ (= x² に比例して y が変形)
//     - X 方向と Y 方向で歪み量が異なるのが普通
//
//   既存 hlsl/distortion (= fs_distortion.sc) は完全に放射対称
//   (`r²=x²+y²`、`xy *= f(r²)`) で、X / Y 別量の糸巻きは作れないため
//   本 shader を別途追加し、distortion pass の前段として直列に挿入する。
//
//   モデル (中心相対 UV を [-1, 1] として):
//     pinch_x = u_vec_pincushion_x.x + u_vec_pincushion_x.y * y²   (cubic 項)
//     pinch_y = u_vec_pincushion_y.x + u_vec_pincushion_y.y * x²
//     x *= 1 + pinch_x * y²
//     y *= 1 + pinch_y * x²
//
//   default 全 0 で identity (歪み無し)。

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_vec_pincushion_x_quad;   // (.x = X 軸 quad 量、正で内側糸巻き)
uniform vec4 u_vec_pincushion_x_cubic;  // (.x = X 軸 cubic 補正、角を強める)
uniform vec4 u_vec_pincushion_y_quad;   // (.x = Y 軸 quad 量)
uniform vec4 u_vec_pincushion_y_cubic;  // (.x = Y 軸 cubic 補正)

// UI slider の ±1.0 は実効値として強すぎるため shader 内で 1/6 にスケールする。
// 粗い slider (±1.0 / step 0.01) の操作感を保ちつつ実効値を抑える。
#define PINCUSHION_GAIN (5.0 / 30.0)

void main()
{
	// UV (0..1) → 中心相対 (-1..1)
	vec2 uv = v_texcoord0 * 2.0 - 1.0;
	float x = uv.x;
	float y = uv.y;

	float y2 = y * y;
	float x2 = x * x;

	// quad + cubic 項を合成 (cubic は y⁴ / x⁴ 項として効く) + 1/30 スケール
	float pinch_x = (u_vec_pincushion_x_quad.x + u_vec_pincushion_x_cubic.x * y2) * PINCUSHION_GAIN;
	float pinch_y = (u_vec_pincushion_y_quad.x + u_vec_pincushion_y_cubic.x * x2) * PINCUSHION_GAIN;

	x *= 1.0 + pinch_x * y2;
	y *= 1.0 + pinch_y * x2;

	// (-1..1) → (0..1) に戻す
	vec2 distorted_uv = (vec2(x, y) + 1.0) * 0.5;

	// 範囲外は黒 (round_corner と同じ扱い)
	if (distorted_uv.x < 0.0 || distorted_uv.x > 1.0 ||
		distorted_uv.y < 0.0 || distorted_uv.y > 1.0)
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	gl_FragColor = texture2D(s_tex, distorted_uv) * v_color0;
}
