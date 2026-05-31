$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// STAR WARS glow 用、4タップ bilinear ボックス downsample
// 線エネルギーが downsample で潰れないようにソース全ピクセルを平均化する。
// 4 タップそれぞれが bilinear で 2x2 ソースをサンプル → 合計 4x4 = 16 source ピクセルをカバー。
//
// u_inv_screen_dims は chain system が第1入力テクスチャの逆数寸法を自動セットする。
// * 0.5 で bilinear half-texel オフセットになり、全 downsample ステージで共通の式になる。

#include "common.sh"

SAMPLER2D(s_tex, 0);

// chain system が入力テクスチャ寸法から自動設定: (1/input_w, 1/input_h, 0, 0)
uniform vec4 u_inv_screen_dims;

void main()
{
	vec2 o = u_inv_screen_dims.xy * 0.5;
	vec4 c = texture2D(s_tex, v_texcoord0 + vec2(-o.x, -o.y))
	       + texture2D(s_tex, v_texcoord0 + vec2( o.x, -o.y))
	       + texture2D(s_tex, v_texcoord0 + vec2(-o.x,  o.y))
	       + texture2D(s_tex, v_texcoord0 + vec2( o.x,  o.y));
	gl_FragColor = c * 0.25 * v_color0;
}
