$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// HLSL bloom.fx 互換の最終 bloom 合成パス。
//   base (= phosphor / post 出力) と bloom_sum (= 15 mip 重み付き合計) を読み、
//   HLSL 原版と同じ overdrive (cross-talk) を計算してから REPLACE blend で書く。
//
//   HLSL bloom.fx:384-391 原版 (overdrive):
//     float3 over = max(0, texel + bloom - 1.0) * BloomOverdrive;
//     bloom.r += (over.g + over.b) * 0.5;
//     bloom.g += (over.r + over.b) * 0.5;
//     bloom.b += (over.r + over.g) * 0.5;
//
//   HLSL bloom.fx:303-307 NoiseFactor (CRT 暗部のノイズ揺れ再現):
//     return 1.0 + random * max(0.0, 0.25 * pow(E, -8 * n));
//
//   HLSL bloom.fx:396 最終合成:
//     blend = texel + bloom * NoiseFactor;
//
// (確定版): bloom_sum target が RGBA16F に変わったため
//   累積 clamp は解消。NoiseFactor も追加。
//   ただしユーザ実測で「BGFX Scale 2.0 = D3D Scale 0.75」(= BGFX が 2.67 倍弱い)
//   が残ったため、UI 上の slider 値を D3D と一致させるため `BLOOM_BRIGHTNESS_GAIN`
//   = 2.67 を入れる。真因 (bilinear filter の D3D9/bgfx 微差? prepare_bloom 内部処理?)
//   は完全解明できなかったが、実測ベースで HLSL D3D と等価 UI 操作を実現する。

#include "common.sh"

SAMPLER2D(s_base,  0);
SAMPLER2D(s_bloom, 1);

uniform vec4 u_bloom_base_weight;  // (base_weight, 0, 0, 0) HLSL Level0Weight 相当、default 1.0
uniform vec4 u_bloom_scale;        // (scale, 0, 0, 0) bloom mip 合計の master scale
uniform vec4 u_bloom_overdrive;    // (r, g, b, 0)
uniform vec4 u_bloom_channel_gain; // (r, g, b, 0) okaz 独自: bloom 寄与の per-channel gain (default 1,1,1)
uniform vec4 u_time;               // (time_seconds, 0, 0, 0) chain system parameter で auto-bind

// HLSL random.fx 互換の per-pixel + per-frame hash random [0, 1]
float random(vec2 uv, float t)
{
	return fract(sin(dot(uv + vec2(t, t), vec2(12.9898, 78.233))) * 43758.5453);
}

// HLSL D3D と UI 上の slider 値を一致させるための実測補正係数
// (BGFX Scale 2.0 ≒ D3D Scale 0.75 → ratio = 2.0/0.75 = 2.67)
#define BLOOM_BRIGHTNESS_GAIN 2.67

// Overdrive も同様に実測で BGFX 0.5 ≒ D3D 1.0 (= BGFX が 2.0 倍弱い) のため補正
// (bloom 自体が BRIGHTNESS_GAIN で 2.67 倍されるため、overdrive 入力の sum_for_overdrive
//  は base+bloom と既に揃っている。残る差分が overdrive 係数側の弱さ → 別途 2.0 倍)
#define BLOOM_OVERDRIVE_GAIN 2.0

void main()
{
	vec3 base  = texture2D(s_base,  v_texcoord0).rgb * u_bloom_base_weight.x;
	vec3 bloom = texture2D(s_bloom, v_texcoord0).rgb * u_bloom_scale.x * BLOOM_BRIGHTNESS_GAIN;
	// okaz 独自: per-channel bloom gain (R/G/B 個別補正、default 1,1,1)
	// CRT/モニタの蛍光体効率差で R/G/B のかかり方が違って見えるのを補正する用途
	bloom *= u_bloom_channel_gain.rgb;

	// HLSL 原版互換 overdrive: base+bloom が 1 を超えた量を cross-talk
	vec3 sum_for_overdrive = base + bloom;
	vec3 over = max(vec3_splat(0.0), sum_for_overdrive - vec3_splat(1.0)) * u_bloom_overdrive.rgb * BLOOM_OVERDRIVE_GAIN;
	// 一時的: overdrive がかかりすぎないよう clamp (各 channel あたり最大 1.0)
	over = min(over, vec3_splat(1.0));
	vec3 crosstalk = vec3(
		(over.g + over.b) * 0.5,
		(over.r + over.b) * 0.5,
		(over.r + over.g) * 0.5
	);
	bloom += crosstalk;

	// HLSL NoiseFactor: 暗部 (n→0) で最大 +25%、明部 (n→1) でほぼ 0 の揺らぎ
	float r = random(v_texcoord0, u_time.x);
	vec3 noise_factor = vec3_splat(1.0) + r * max(vec3_splat(0.0), vec3_splat(0.25) * exp(-8.0 * bloom));
	bloom *= noise_factor;

	gl_FragColor = vec4(base + bloom, 1.0);
}
