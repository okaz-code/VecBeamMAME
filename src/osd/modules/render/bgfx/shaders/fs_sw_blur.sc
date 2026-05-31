$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// / : STAR WARS glow 用、25 タップ可分ガウシアンブラー
// 25 タップ（中央 + 12×2）σ≈5 で隙間出ない上限を Width≈4 まで広げた。
//
// u_blur_dir.xy:      方向。水平 = (1,0)、垂直 = (0,1)。vector-starwars.json の per-pass uniforms で設定。
// u_blur_width.x:     Halo Width slider。タップ間隔倍率。
// u_blur_strength.xyz: Halo Strength color slider (R/G/B 個別)。最終輝度乗算 (チャネル別)。
// u_inv_screen_dims:  chain system が入力テクスチャ寸法から自動セット (1/in_w, 1/in_h)

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_inv_screen_dims;
uniform vec4 u_blur_dir;
uniform vec4 u_blur_width;
uniform vec4 u_blur_strength;

void main()
{
	vec2 d = u_blur_dir.xy * u_blur_width.x * u_inv_screen_dims.xy;

	// σ=5 の正規化ガウシアン重み (sum ≒ 1.0)
	vec4 result = texture2D(s_tex, v_texcoord0) * 0.0808
		+ (texture2D(s_tex, v_texcoord0 + d *  1.0) + texture2D(s_tex, v_texcoord0 - d *  1.0)) * 0.0792
		+ (texture2D(s_tex, v_texcoord0 + d *  2.0) + texture2D(s_tex, v_texcoord0 - d *  2.0)) * 0.0746
		+ (texture2D(s_tex, v_texcoord0 + d *  3.0) + texture2D(s_tex, v_texcoord0 - d *  3.0)) * 0.0675
		+ (texture2D(s_tex, v_texcoord0 + d *  4.0) + texture2D(s_tex, v_texcoord0 - d *  4.0)) * 0.0587
		+ (texture2D(s_tex, v_texcoord0 + d *  5.0) + texture2D(s_tex, v_texcoord0 - d *  5.0)) * 0.0490
		+ (texture2D(s_tex, v_texcoord0 + d *  6.0) + texture2D(s_tex, v_texcoord0 - d *  6.0)) * 0.0393
		+ (texture2D(s_tex, v_texcoord0 + d *  7.0) + texture2D(s_tex, v_texcoord0 - d *  7.0)) * 0.0303
		+ (texture2D(s_tex, v_texcoord0 + d *  8.0) + texture2D(s_tex, v_texcoord0 - d *  8.0)) * 0.0225
		+ (texture2D(s_tex, v_texcoord0 + d *  9.0) + texture2D(s_tex, v_texcoord0 - d *  9.0)) * 0.0160
		+ (texture2D(s_tex, v_texcoord0 + d * 10.0) + texture2D(s_tex, v_texcoord0 - d * 10.0)) * 0.0109
		+ (texture2D(s_tex, v_texcoord0 + d * 11.0) + texture2D(s_tex, v_texcoord0 - d * 11.0)) * 0.0072
		+ (texture2D(s_tex, v_texcoord0 + d * 12.0) + texture2D(s_tex, v_texcoord0 - d * 12.0)) * 0.0045;

	vec4 tinted = result * v_color0;
	gl_FragColor = vec4(tinted.rgb * u_blur_strength.xyz, tinted.a);
}
