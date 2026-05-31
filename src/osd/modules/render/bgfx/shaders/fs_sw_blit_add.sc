$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// ADD blend で入力をコピーしつつ u_blit_intensity.x で乗算。
// halo / bloom を target に加算合成する用途。
//
// MAME HLSL bloom.fx の Bloom Overdrive (色 cross-talk) を移植。
//   原理: bloom が明るいチャネルは他チャネルへ「色漏れ」する CRT 蛍光体相互干渉の再現。
//   原版 (bloom.fx:384-391):
//     float3 over = max(0, texel + bloom - 1.0) * BloomOverdrive;
//     bloom.r += (over.g + over.b) * 0.5;  // G/B のオーバー量を R へ
//     bloom.g += (over.r + over.b) * 0.5;
//     bloom.b += (over.r + over.g) * 0.5;
//   私の mod は BGRA8 のため base+bloom-1 を厳密に取れないので近似:
//   bloom 自身の閾値超過分 (max(0, bloom - 0.5)) を相互漏らす。
//   default u_bloom_overdrive = (0,0,0) で無効、(1,1,1) で全チャネル cross-talk 強。

#include "common.sh"

SAMPLER2D(s_tex, 0);

uniform vec4 u_blit_intensity;
uniform vec4 u_bloom_overdrive;  // (r, g, b, 0) default (0,0,0,0)

void main()
{
	vec3 bloom = texture2D(s_tex, v_texcoord0).rgb;

	// Bloom Overdrive: bloom が 0.5 を超える量を他チャネルに漏らす近似 cross-talk
	// (HLSL は float3(scalar) 単一引数 splat を許さないため vec3_splat マクロ使用)
	vec3 excess = max(vec3_splat(0.0), bloom - vec3_splat(0.5)) * u_bloom_overdrive.rgb;
	vec3 crosstalk = vec3(
		(excess.g + excess.b) * 0.5,
		(excess.r + excess.b) * 0.5,
		(excess.r + excess.g) * 0.5
	);
	bloom += crosstalk;

	gl_FragColor = vec4(bloom * u_blit_intensity.x, 1.0) * v_color0;
}
