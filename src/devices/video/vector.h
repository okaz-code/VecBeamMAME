// license:BSD-3-Clause
// copyright-holders:Brad Oliver,Aaron Giles,Bernd Wiebelt,Allard van der Bas
#ifndef MAME_VIDEO_VECTOR_H
#define MAME_VIDEO_VECTOR_H

#pragma once

#include "notifier.h"

#include <utility>


class vector_device;

class vector_options
{
public:
	friend class vector_device;

	static float s_flicker;
	static float s_flicker_ini;   // ini 由来 flicker default
	// 原 MAME との差分: オーバースキャン zoom 倍率 (chain slider で注入、中心 0.5 を基準に縮小)
	static float s_overscan_x;
	static float s_overscan_y;
	static float s_beam_width_min;
	static float s_beam_width_max;
	// ini 由来の beam_width default (chain slider 不在時の fallback)
	static float s_beam_width_min_ini;
	static float s_beam_width_max_ini;
	static float s_beam_dot_size;
	static float s_beam_dot_size_ini;   // ini 由来 dot size default (chain slider fallback)
	static float s_beam_intensity_weight;   // 標準 MAME UI slider (ui.cpp) が参照
	// 点ベクター (長さ 0 の VCTR) 専用サイズ。line の beam_width とは独立で、点の intensity に
	// 応じて min..max を curve (sigmoid k) で補間する。chain slider が毎フレーム更新。
	static float s_beam_dot_size_min;
	static float s_beam_dot_size_max;
	static float s_beam_dot_size_curve;

	// 全 vector ゲーム共通の overload パラメータ (drawbgfx slider が毎フレーム更新)。
	// 入力 (raw_score / 1751.85 か intensity / 255) → sigmoid → 出力値 (0..1) →
	//   threshold 以下: display_intensity = out/thr * 255、overload = 0
	//   threshold 超え: display_intensity = 255、overload = (out - thr) / (1 - thr)
	static float s_overload_sigmoid_k;        // 入力 → 出力 値変換 sigmoid 急峻度 (-1..1)
	static float s_overload_threshold;        // 出力値 (0..1) の overload 閾値 (1.0 = overload 発生せず)
	static float s_overload_width_curve_low;  // 0..threshold の太さ curve k
	static float s_overload_width_curve_high; // threshold..1 の太さ curve k
	static float s_overload_width_max;        // overload 最大太さ倍率

	// Monitor Glow 用 (drawbgfx が前 3 個を更新、vector.cpp が
	// s_monitor_glow_amount を計算、drawbgfx が読込)
	// 「画面外に飛び出して、かつモニタ枠から外方向にこの距離を超えた overload 線」のみ集計。
	static float s_mglow_threshold;         // overload > これだけ集計対象
	static float s_mglow_min_distance;      // 画面端から外方向のこの距離 (normalized) 超で集計
	static float s_mglow_coefficient;       // 集計倍率
	static float s_monitor_glow_amount;     // 当該フレーム集計結果

	// ベクター CRT フリッカー再現用
	// 0.0 = 機能無効 (毎 vsync 同じ list を再描画、現在の MAME 既定挙動)
	// 1.0 = 完全スキップ (AVG が新フレームを完成させていない vsync は線を描かない →
	//       chain phosphor が自然減衰してフリッカー発生)
	// 中間値 = 線輝度を線形減衰 (滑らかな段階的フェード)
	static float s_vector_flicker;

protected:
	static void init(emu_options& options);
};

class vector_device : public device_t, public device_video_interface
{
public:
	using frame_begin_delegate = delegate<void ()>;
	using frame_end_delegate = delegate<void ()>;
	using move_delegate = delegate<void (int, int, uint32_t, int, int)>;
	using line_delegate = delegate<void (int, int, int, int, uint32_t, int, int, int)>;

	template <typename T> static constexpr rgb_t color111(T c) { return rgb_t(pal1bit(c >> 2), pal1bit(c >> 1), pal1bit(c >> 0)); }
	template <typename T> static constexpr rgb_t color222(T c) { return rgb_t(pal2bit(c >> 4), pal2bit(c >> 2), pal2bit(c >> 0)); }
	template <typename T> static constexpr rgb_t color444(T c) { return rgb_t(pal4bit(c >> 8), pal4bit(c >> 4), pal4bit(c >> 0)); }

	// construction/destruction
	vector_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	uint32_t screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void clear_list();

	void add_point(int x, int y, rgb_t color, int intensity);
	// STAR WARS 専用、raw_score (VCTR_x * STAT_intensity) を伴う
	void add_point_sw(int x, int y, rgb_t color, int intensity, uint16_t raw_score);

	// device-level overrides
	virtual void device_start() override ATTR_COLD;

	// notifiers
	util::notifier_subscription add_frame_begin_notifier(frame_begin_delegate &&n);
	template <typename T>
	util::notifier_subscription add_frame_begin_notifier(T &&n)
	{ return add_frame_begin_notifier(frame_begin_delegate(std::forward<T>(n))); }

	util::notifier_subscription add_frame_end_notifier(frame_end_delegate &&n);
	template <typename T>
	util::notifier_subscription add_frame_end_notifier(T &&n)
	{ return add_frame_end_notifier(frame_end_delegate(std::forward<T>(n))); }

	util::notifier_subscription add_move_notifier(move_delegate &&n);
	template <typename T>
	util::notifier_subscription add_move_notifier(T &&n)
	{ return add_move_notifier(move_delegate(std::forward<T>(n))); }

	util::notifier_subscription add_line_notifier(line_delegate &&n);
	template <typename T>
	util::notifier_subscription add_line_notifier(T &&n)
	{ return add_line_notifier(line_delegate(std::forward<T>(n))); }

private:
	float normalized_sigmoid(float n, float k);

	/* The vertices are buffered here */
	struct point
	{
		point() : x(0), y(0), col(0), intensity(0), raw_score(0) { }

		int x; int y;
		rgb_t col;
		int intensity;
		// STAR WARS 用、VCTR_x * STAT_intensity 由来の生スコア (0..1752)。
		// 0 の時は STAR WARS 以外（既存パス）。
		uint16_t raw_score;
	};

	std::unique_ptr<point[]> m_vector_list;
	int m_vector_index;
	int m_min_intensity;
	int m_max_intensity;

	// フリッカー検出用世代カウンタ
	// clear_list() / add_point*() が呼ばれるたびに inc。
	// screen_update() で m_last_drawn_generation と比較し、変化が無ければ "stale"
	// (= AVG が新フレームを完成させていない) と判定し、line 描画をスキップ/減衰させる。
	uint32_t m_list_generation;
	uint32_t m_last_drawn_generation;

	// notify interested parties about vector-drawing activities
	util::notifier<> m_frame_begin_notifier;
	util::notifier<> m_frame_end_notifier;
	util::notifier<int, int, uint32_t, int, int> m_move_notifier;
	util::notifier<int, int, int, int, uint32_t, int, int, int> m_line_notifier;
};

// device type definition
DECLARE_DEVICE_TYPE(VECTOR, vector_device)

// device iterator
typedef device_type_enumerator<vector_device> vector_device_enumerator;

#endif // MAME_VIDEO_VECTOR_H
