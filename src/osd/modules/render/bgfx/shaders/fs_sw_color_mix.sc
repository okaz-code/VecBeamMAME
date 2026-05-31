$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// 3x3 RGB color matrix + post-mix channel gain。仕様:
//
//   1) Color Matrix (3x3, 各成分 0..1):
//        u_red_mix.rgb    = 入力 R → 出力 (R, G, B) への寄与   default (1, 0, 0)
//        u_green_mix.rgb  = 入力 G → 出力 (R, G, B) への寄与   default (0, 1, 0)
//        u_blue_mix.rgb   = 入力 B → 出力 (R, G, B) への寄与   default (0, 0, 1)
//
//      default は単位行列 = identity (色変換無し)。
//      使用例:
//        - "Red Mix Blue" を 0.5 にすると、ゲーム内の赤線が紫がかって表示される
//          (Red 入力 1.0 → 出力 (R=1, G=0, B=0.5))
//        - 純白 (1,1,1) は行列の各列和が反映される
//        - すべて [0..1] のため減算は不可、混色のみ
//
//   2) Line Channel Gain (R/G/B, 0..2, default 1.0):
//        u_line_channel_gain.rgb で post-matrix の輝度を増幅 (青ブースト等)。
//        post-mix の最終ゲインなので min クランプの影響を受けず gain>1 で実際に明るくなる。

#include "common.sh"

SAMPLER2D(s_tex, 0);
uniform vec4 u_red_mix;            // (R→R, R→G, R→B, 0) default (1,0,0,0)
uniform vec4 u_green_mix;          // (G→R, G→G, G→B, 0) default (0,1,0,0)
uniform vec4 u_blue_mix;           // (B→R, B→G, B→B, 0) default (0,0,1,0)
uniform vec4 u_line_channel_gain;  // (r, g, b, 0)        default (1,1,1,0)

void main()
{
	vec4 c = texture2D(s_tex, v_texcoord0);

	// 3x3 color matrix
	vec3 mixed = c.r * u_red_mix.rgb
	           + c.g * u_green_mix.rgb
	           + c.b * u_blue_mix.rgb;

	// post-mix channel gain (post-saturation boost)
	mixed *= u_line_channel_gain.rgb;
	mixed = clamp(mixed, 0.0, 1.0);

	gl_FragColor = vec4(mixed, c.a) * v_color0;
}
