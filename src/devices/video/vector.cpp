// license:BSD-3-Clause
// copyright-holders:Brad Oliver,Aaron Giles,Bernd Wiebelt,Allard van der Bas
/******************************************************************************
 *
 * vector.cpp
 *
 *        anti-alias code by Andrew Caldwell
 *        (still more to add)
 *
 * Vector Team
 *
 *        Brad Oliver
 *        Aaron Giles
 *        Bernd Wiebelt
 *        Allard van der Bas
 *        Al Kossow (VECSIM)
 *        Hedley Rainnie (VECSIM)
 *        Eric Smith (VECSIM)
 *        Neil Bradley (technical advice)
 *        Andrew Caldwell (anti-aliasing)
 *
 **************************************************************************** */

#include "emu.h"
#include "vector.h"

#include "emuopts.h"
#include "render.h"
#include "screen.h"


#define VECTOR_WIDTH_DENOM 512

// 20000 is needed for mhavoc (see MT 06668) 10000 is enough for other games
#define MAX_POINTS 20000

float vector_options::s_flicker = 0.0f;
// ini 由来 flicker default (chain slider "vector_fake_flicker" 不在時の fallback)
float vector_options::s_flicker_ini = 0.0f;
// 原 MAME との差分: オーバースキャン zoom 倍率 (default 1.0 = 等倍。ini option は無く chain slider のみ)
float vector_options::s_overscan_x = 1.0f;
float vector_options::s_overscan_y = 1.0f;
float vector_options::s_beam_width_min = 0.0f;
float vector_options::s_beam_width_max = 0.0f;
// chain JSON slider で beam_width を上書きする際の fallback。
// ini 由来 default を保持し、slider 不在 chain では ini 値に戻す。
float vector_options::s_beam_width_min_ini = 0.0f;
float vector_options::s_beam_width_max_ini = 0.0f;
float vector_options::s_beam_dot_size = 0.0f;
float vector_options::s_beam_dot_size_ini = 0.0f;   // ini 由来 dot size default
float vector_options::s_beam_intensity_weight = 0.0f;   // 標準 MAME UI slider (ui.cpp) が参照
// 点ベクター専用サイズ (drawbgfx slider が毎フレーム上書き)。beam_width とは独立。
float vector_options::s_beam_dot_size_min = 0.1f;
float vector_options::s_beam_dot_size_max = 0.5f;
float vector_options::s_beam_dot_size_curve = 0.0f;

// 全 vector ゲーム共通 overload デフォルト値。
// drawbgfx の slider が毎フレーム上書きする。slider 接続前は overload 発生せず安全。
float vector_options::s_overload_sigmoid_k       = 0.0f; // 0 = 線形
float vector_options::s_overload_threshold       = 1.0f; // 1.0 = overload 発生せず (chain JSON 側で 0.5 にすると half で overload)
float vector_options::s_overload_width_curve_low = 0.0f; // 0 = 線形
float vector_options::s_overload_width_curve_high= 0.0f; // 0 = 線形
float vector_options::s_overload_width_max       = 5.0f; // overload 最大太さ

// Monitor Glow 用デフォルト (drawbgfx slider が毎フレーム上書き)
float vector_options::s_mglow_threshold    = 0.5f;
float vector_options::s_mglow_min_distance = 0.10f;
float vector_options::s_mglow_coefficient  = 0.0f;    // 0 で機能 off (drawbgfx が 1.0 等を設定)
float vector_options::s_monitor_glow_amount = 0.0f;

// ベクター CRT フリッカー再現 (デフォルト 0 = 機能 off、drawbgfx slider が上書き)
float vector_options::s_vector_flicker = 0.0f;

void vector_options::init(emu_options &options)
{
	s_beam_width_min = options.beam_width_min();
	s_beam_width_max = options.beam_width_max();
	s_beam_width_min_ini = options.beam_width_min();   // chain slider 不在時の fallback
	s_beam_width_max_ini = options.beam_width_max();
	s_beam_dot_size = options.beam_dot_size();
	s_beam_dot_size_ini = options.beam_dot_size();   // chain slider 不在時の fallback
	s_beam_intensity_weight = options.beam_intensity_weight();
	s_flicker = options.flicker();
	s_flicker_ini = options.flicker();   // chain slider 不在時の fallback
}

// device type definition
DEFINE_DEVICE_TYPE(VECTOR, vector_device, "vector_device", "VECTOR")

vector_device::vector_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, VECTOR, tag, owner, clock),
		device_video_interface(mconfig, *this),
		m_vector_list(nullptr),
		m_min_intensity(255),
		m_max_intensity(0),
		// フリッカー検出用カウンタ
		// 初期値は m_list_generation = 0 / m_last_drawn_generation = ~0 で「初回 vsync 必ず描画」
		m_list_generation(0),
		m_last_drawn_generation(~uint32_t(0))
{
}

void vector_device::device_start()
{
	vector_options::init(machine().options());

	m_vector_index = 0;

	/* allocate memory for tables */
	m_vector_list = std::make_unique<point[]>(MAX_POINTS);
}


//-------------------------------------------------
//  subscribe for frame-begin notifications
//-------------------------------------------------

util::notifier_subscription vector_device::add_frame_begin_notifier(frame_begin_delegate &&n)
{
	return m_frame_begin_notifier.subscribe(std::move(n));
}


//-------------------------------------------------
//  subscribe for frame-end notifications
//-------------------------------------------------

util::notifier_subscription vector_device::add_frame_end_notifier(frame_end_delegate &&n)
{
	return m_frame_end_notifier.subscribe(std::move(n));
}


//-------------------------------------------------
//  subscribe for hidden-move notifications
//-------------------------------------------------

util::notifier_subscription vector_device::add_move_notifier(move_delegate &&n)
{
	return m_move_notifier.subscribe(std::move(n));
}


//-------------------------------------------------
//  subscribe for visible-line notifications
//-------------------------------------------------

util::notifier_subscription vector_device::add_line_notifier(line_delegate &&n)
{
	return m_line_notifier.subscribe(std::move(n));
}


//-------------------------------------------------
// www.dinodini.wordpress.com/2010/04/05/normalized-tunable-sigmoid-functions/
//-------------------------------------------------

float vector_device::normalized_sigmoid(float n, float k)
{
	// valid for n and k in range of -1.0 and 1.0
	return (n - n * k) / (k - fabs(n) * 2.0f * k + 1.0f);
}


//-------------------------------------------------
// Adds a line end point to the vertices list. The vector processor emulation
// needs to call this.
//-------------------------------------------------

void vector_device::add_point(int x, int y, rgb_t color, int intensity)
{
	point *newpoint;

	// 世代 inc (フリッカー検出用)。clear_list() 以外の経路 (例
	// AVG 以外の vector ハードウェア) で list が更新された場合のセーフティ。
	m_list_generation++;

	intensity = std::clamp(intensity, 0, 255);

	// overload線分 (col.a() > 0) は通常線分の min/max 追跡から除外する。
	// overload時の高い draw_intensity が m_max_intensity を押し上げ、
	// 同一フレーム内の通常線分まで太くなるのを防ぐため。
	if (color.a() == 0 && intensity > 0)
	{
		m_min_intensity = std::min(m_min_intensity, intensity);
		m_max_intensity = std::max(m_max_intensity, intensity);
	}

	if (vector_options::s_flicker && (intensity > 0))
	{
		float random = float(machine().rand() & 255) / 255.0f; // random value between 0.0 and 1.0

		intensity -= int(intensity * random * vector_options::s_flicker);

		intensity = std::clamp(intensity, 0, 255);
	}

	newpoint = &m_vector_list[m_vector_index];
	newpoint->x = x;
	newpoint->y = y;
	newpoint->col = color;
	newpoint->intensity = intensity;
	newpoint->raw_score = 0;  // 既存パス: STAR WARS 専用フィールドはゼロ

	m_vector_index++;
	if (m_vector_index >= MAX_POINTS)
	{
		m_vector_index--;
		logerror("*** Warning! Vector list overflow!\n");
	}
}


// STAR WARS 専用 add_point。raw_score (VCTR_x * STAT_intensity) を
// 一緒に格納する。screen_update が raw_score > 0 を見て STAR WARS パスに分岐。
// フリッカー検出用に世代 inc を追加。
void vector_device::add_point_sw(int x, int y, rgb_t color, int intensity, uint16_t raw_score)
{
	point *newpoint;

	// 世代 inc (フリッカー検出用)
	m_list_generation++;

	intensity = std::clamp(intensity, 0, 255);

	// STAR WARS パスでは min/max 追跡を使わない（独自の sigmoid + threshold で計算するため）。
	// 通常パスへの干渉を防ぐため m_min_intensity/m_max_intensity には書き込まない。

	if (vector_options::s_flicker && (intensity > 0))
	{
		float random = float(machine().rand() & 255) / 255.0f;
		intensity -= int(intensity * random * vector_options::s_flicker);
		intensity = std::clamp(intensity, 0, 255);
	}

	newpoint = &m_vector_list[m_vector_index];
	newpoint->x = x;
	newpoint->y = y;
	newpoint->col = color;
	newpoint->intensity = intensity;
	newpoint->raw_score = raw_score;

	m_vector_index++;
	if (m_vector_index >= MAX_POINTS)
	{
		m_vector_index--;
		logerror("*** Warning! Vector list overflow!\n");
	}
}


//-------------------------------------------------
// The vector CPU creates a new display list. We save the old display list,
// but only once per refresh.
//-------------------------------------------------

void vector_device::clear_list()
{
	m_vector_index = 0;
#if 1	// e
	m_min_intensity = 255;
	m_max_intensity = 0;
#endif
	// 「AVG が新フレーム開始」の主シグナル。世代 inc。
	m_list_generation++;
}

//-------------------------------------------------
// Update the screen container with queued vectors.
//-------------------------------------------------

uint32_t vector_device::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	uint32_t flags = PRIMFLAG_ANTIALIAS(1) | PRIMFLAG_BLENDMODE(BLENDMODE_ADD) | PRIMFLAG_VECTOR(1);
	const rectangle &visarea = screen.visible_area();
	float xscale = 1.0f / (65536 * visarea.width());
	float yscale = 1.0f / (65536 * visarea.height());
	float xoffs = (float)visarea.min_x;
	float yoffs = (float)visarea.min_y;

	point *curpoint;
	int lastx = 0;
	int lasty = 0;

	curpoint = m_vector_list.get();

	screen.container().empty();
	screen.container().add_rect(0.0f, 0.0f, 1.0f, 1.0f, rgb_t(0xff,0x00,0x00,0x00), PRIMFLAG_BLENDMODE(BLENDMODE_ALPHA) | PRIMFLAG_VECTORBUF(1));

	m_frame_begin_notifier();

	// 実機ベクター CRT フリッカー再現
	// stale = AVG が新フレームを完成させていない vsync。
	// vector_flicker slider に応じて線輝度を減衰、ないし完全スキップ。
	//   draw_factor = stale 時 (1 - vector_flicker)、非 stale 時 1.0
	//   0.0 → 線描画なし → chain phosphor が自然減衰
	//   1.0 → 通常描画 (フリッカー無効と同等)
	//   中間値 → 部分減衰 (滑らかなフェード)
	const bool is_stale = (m_list_generation == m_last_drawn_generation);
	m_last_drawn_generation = m_list_generation;
	const float draw_factor = is_stale
		? std::max(0.0f, 1.0f - vector_options::s_vector_flicker)
		: 1.0f;

	// Monitor Glow 集計をフレーム冒頭でリセット。
	// 線ループ内で off-screen + edge から外方向に min_distance 超の overload を集計する。
	vector_options::s_monitor_glow_amount = 0.0f;
	const float mg_thr   = vector_options::s_mglow_threshold;
	const float mg_mind  = vector_options::s_mglow_min_distance;
	const float mg_coeff = vector_options::s_mglow_coefficient;

	// 完全スキップ (draw_factor ≈ 0)。背景 rect + frame notifier だけ実行して return。
	if (draw_factor <= 0.001f)
	{
		m_frame_end_notifier();
		return 0;
	}

	for (int i = 0; i < m_vector_index; i++)
	{
		render_bounds coords;

		// 全 vector ゲーム共通 overload パイプライン
		// 入力ソース (0..1):
		//   raw_score > 0 (STAR WARS): VCTR_x * STAT_intensity を 1751.85 で正規化
		//   それ以外: intensity を 255 で正規化 (既存パスと同等)
		// 共通処理: sigmoid (overload_sigmoid_k) → 出力値 (0..1) → threshold で
		//   display / overload に分離 → 太さは curve_low / curve_high の sigmoid で補間
		float beam_width;
		float overload_val;
		int display_intensity;  // 0..255

		const float src_val = (curpoint->raw_score > 0)
			? float(curpoint->raw_score) / 1751.85f
			: float(curpoint->intensity) / 255.0f;

		const float sig = normalized_sigmoid(src_val, vector_options::s_overload_sigmoid_k);
		const float out_val = std::clamp(sig, 0.0f, 1.0f);
		const float thr = std::clamp(vector_options::s_overload_threshold, 0.001f, 1.0f);

		if (out_val <= thr)
		{
			const float t = out_val / thr;  // 0..1
			display_intensity = int(t * 255.0f + 0.5f);
			overload_val = 0.0f;
			// 太さ: min..max を curve_low の sigmoid で補間
			const float w = std::clamp(
				normalized_sigmoid(t, vector_options::s_overload_width_curve_low), 0.0f, 1.0f);
			beam_width = vector_options::s_beam_width_min +
				w * (vector_options::s_beam_width_max - vector_options::s_beam_width_min);
		}
		else
		{
			display_intensity = 255;
			const float over_span = std::max(0.001f, 1.0f - thr);
			const float over = std::clamp((out_val - thr) / over_span, 0.0f, 1.0f);
			overload_val = over;
			// 太さ: max..overload_max を curve_high の sigmoid で補間
			const float w = std::clamp(
				normalized_sigmoid(over, vector_options::s_overload_width_curve_high), 0.0f, 1.0f);
			beam_width = vector_options::s_beam_width_max +
				w * (vector_options::s_overload_width_max - vector_options::s_beam_width_max);
		}
		beam_width *= 1.0f / (float)VECTOR_WIDTH_DENOM;

		// 点ベクター (長さ 0): beam_width とは無関係に、点の intensity (src_val 0..1) で
		// dot_size_min..max を curve (sigmoid k) 補間したサイズで描く。
		if (lastx == curpoint->x && lasty == curpoint->y)
		{
			const float w = std::clamp(
				normalized_sigmoid(std::clamp(src_val, 0.0f, 1.0f), vector_options::s_beam_dot_size_curve),
				0.0f, 1.0f);
			const float dot = vector_options::s_beam_dot_size_min +
				w * (vector_options::s_beam_dot_size_max - vector_options::s_beam_dot_size_min);
			beam_width = std::max(0.0f, dot) * (1.0f / (float)VECTOR_WIDTH_DENOM);
		}

		coords.x0 = (float(lastx) - xoffs) * xscale;
		coords.y0 = (float(lasty) - yoffs) * yscale;
		coords.x1 = (float(curpoint->x) - xoffs) * xscale;
		coords.y1 = (float(curpoint->y) - yoffs) * yscale;

		// オーバースキャン zoom。中心 0.5 で X/Y 独立に縮小 (0.33..1.0)。
		// zoom<1 で content が中心に縮み、visarea 外のベクターが 0..1 内に入って表示される。
		if (vector_options::s_overscan_x != 1.0f || vector_options::s_overscan_y != 1.0f)
		{
			coords.x0 = (coords.x0 - 0.5f) * vector_options::s_overscan_x + 0.5f;
			coords.y0 = (coords.y0 - 0.5f) * vector_options::s_overscan_y + 0.5f;
			coords.x1 = (coords.x1 - 0.5f) * vector_options::s_overscan_x + 0.5f;
			coords.y1 = (coords.y1 - 0.5f) * vector_options::s_overscan_y + 0.5f;
		}

		if (curpoint->intensity != 0)
		{
			// Monitor Glow 集計を全 vector ゲーム共通化。
			// overload_val は overload 共通パイプラインで全ゲーム計算済み。
			// 線分のうち**最も画面外に飛び出している**端点の距離を採用 (max norm)。
			// d_far > min_distance なら集計対象 (画面外かつ edge から十分外)。
			if (overload_val > mg_thr && mg_coeff > 0.0f)
			{
				auto outside_dist = [](float x, float y) -> float {
					float dx = 0.0f, dy = 0.0f;
					if (x < 0.0f) dx = -x;
					else if (x > 1.0f) dx = x - 1.0f;
					if (y < 0.0f) dy = -y;
					else if (y > 1.0f) dy = y - 1.0f;
					return std::max(dx, dy);
				};
				const float d0 = outside_dist(coords.x0, coords.y0);
				const float d1 = outside_dist(coords.x1, coords.y1);
				const float d_far = std::max(d0, d1);
				if (d_far > mg_mind)
				{
					// stale frame では monitor glow も減衰させる
					vector_options::s_monitor_glow_amount +=
						(overload_val - mg_thr) * mg_coeff * draw_factor;
				}
			}

			// STAR WARS パスは display_intensity / overload_val を使う、
			// 既存パスは curpoint->intensity / col.a() 由来 (上の if 分岐内で計算済み)。
			// stale frame では draw_factor で intensity を減衰 (フリッカー再現)
			const int eff_intensity = (draw_factor < 0.999f)
				? std::clamp(int(float(display_intensity) * draw_factor + 0.5f), 0, 255)
				: display_intensity;
			screen.container().add_line(
					coords.x0, coords.y0, coords.x1, coords.y1,
					beam_width,
					(uint32_t(eff_intensity) << 24) | (curpoint->col & 0xffffff),
					flags,
					overload_val);
			m_line_notifier(lastx, lasty, curpoint->x, curpoint->y, curpoint->col, curpoint->intensity, visarea.width(), visarea.height());
		}
		else
		{
			m_move_notifier(curpoint->x, curpoint->y, curpoint->col, visarea.width(), visarea.height());
		}

		lastx = curpoint->x;
		lasty = curpoint->y;

		curpoint++;
	}

	m_frame_end_notifier();

	return 0;
}
