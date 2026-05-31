$input v_color0, v_texcoord0

// license:BSD-3-Clause
// copyright-holders:okaz-code
// STAR WARS Monitor Glow
// overload 線が画面端付近に集中したとき、モニター全体を中心明るく光らせる。
//
// u_mglow_amount.x:    CPU 集計値（drawbgfx.cpp が毎フレーム inject）
// u_mglow_brightness.x: Monitor Glow Brightness slider（全体倍率、default 0.5）
// u_mglow_edge_diff.x:  Monitor Glow Center-Edge Diff slider（端の輝度減少量、default 0.5）

#include "common.sh"

uniform vec4 u_mglow_amount;
uniform vec4 u_mglow_brightness;
uniform vec4 u_mglow_edge_diff;

void main()
{
	vec2 d = v_texcoord0 - vec2(0.5, 0.5);
	// 0 (中央) → 1 (四隅)、対角長 = √2 / 2
	float r = clamp(length(d) * 1.41421356, 0.0, 1.0);

	// 中央 1.0、端 (1.0 - edge_diff) で線形補間
	float brightness = mix(1.0, 1.0 - u_mglow_edge_diff.x, r);
	float intensity = u_mglow_amount.x * u_mglow_brightness.x * brightness;

	gl_FragColor = vec4(intensity, intensity, intensity, 1.0) * v_color0;
}
