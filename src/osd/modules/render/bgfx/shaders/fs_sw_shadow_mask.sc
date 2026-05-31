$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// カラーベクター用 shadow mask シェーダ。
// shadow mask テクスチャ (s_mask、aperture_1_2_bgr.png 等) を tile-repeat して
// 入力カラー (s_tex) に乗算。実機のカラー vector CRT が shadow mask 越しに
// RGB 蛍光体を励起する挙動を擬似する。
//
// u_shadow_mask_strength.x:    strength    (0..1、0 = 無効、1 = mask 完全適用)
// u_shadow_mask_brightboost.x: brightboost (0..2、mask 暗化を補正)
// u_shadow_mask_scale.x:       scale       (mask タイル pixel サイズ、1 が最細)
// u_target_dims.xy:            出力ターゲット pixel 寸法 (chain system が自動設定)

#include "common.sh"

SAMPLER2D(s_tex,  0);
SAMPLER2D(s_mask, 1);

uniform vec4 u_shadow_mask_strength;
uniform vec4 u_shadow_mask_brightboost;
uniform vec4 u_shadow_mask_scale;
uniform vec4 u_target_dims;

void main()
{
	vec4 base = texture2D(s_tex, v_texcoord0);

	float strength    = clamp(u_shadow_mask_strength.x,    0.0, 1.0);
	float brightboost = clamp(u_shadow_mask_brightboost.x, 0.0, 2.0);
	float slider_scale = max(1.0, u_shadow_mask_scale.x);

	// 1920×1080 基準で解像度に比例スケール。
	// slider_scale = N は「1080p で N pixel pitch」を意味し、
	// 他解像度でも画面相対サイズが一定になる。
	const float ref_width = 1920.0;
	float scale = slider_scale * (u_target_dims.x / ref_width);

	// pixel 座標に変換 → scale で割って mask UV (tile repeat)
	// sampler の wrap モードが REPEAT なので UV が 1 を超えても自動でタイル化される
	vec2 mask_uv = v_texcoord0 * u_target_dims.xy / scale;
	vec3 mask = texture2D(s_mask, mask_uv).rgb;

	// brightboost を mask 側に適用してから strength で補間。
	// 旧コードは mix 後に brightboost を掛けていたため strength=0 でも全体が 1+brightboost
	// 倍に明るくなってしまうバグがあった fix)。
	//   strength=0 → mask_factor = (1,1,1)                        → 完全 identity
	//   strength=1 → mask_factor = mask * (1 + brightboost)       → 補正済み mask 適用
	vec3 boosted_mask = mask * (1.0 + brightboost);
	vec3 mask_factor  = mix(vec3(1.0, 1.0, 1.0), boosted_mask, strength);

	gl_FragColor = vec4(base.rgb * mask_factor, base.a) * v_color0;
}
