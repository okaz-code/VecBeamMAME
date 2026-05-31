$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// STAR WARS ベクター線用、解析的 AA フラグメントシェーダー
// Overload (focus defocus) 対応
//   v_texcoord0.x = 線単位の overload 値 [0..1]（put_solid_line が prim->overload を格納）
//   v_texcoord0.y = 線幅方向 [0..1]、0.5 が中央

#include "common.sh"

// u_line_params.x: Overload Softness multiplier (slider, default 1.0)
uniform vec4 u_line_params;

void main()
{
	float d = 2.0 * abs(v_texcoord0.y - 0.5);  // 0 (中央) → 1 (端)
	float ovld = v_texcoord0.x;                // per-line overload [0..1]

	// 通常: parabola (中央 1.0、端 0.0、シャープ)
	float fade_sharp = max(0.0, 1.0 - d * d);

	// Overload: Gaussian 風 (緩やかな広がり、エッジ柔らかい、焦点ボケ)
	// k 大 = タイトな Gaussian (sharp)、k 小 = 広い Gaussian (soft)
	float k = mix(8.0, 2.0, clamp(ovld * u_line_params.x, 0.0, 1.0));
	float fade_soft = exp(-d * d * k);

	// overload 0 → sharp、overload 1 → soft で線形補間
	float fade = mix(fade_sharp, fade_soft, ovld);

	gl_FragColor = v_color0 * vec4(1.0, 1.0, 1.0, fade);
}
