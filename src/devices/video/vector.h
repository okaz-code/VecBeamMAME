// license:BSD-3-Clause
// copyright-holders:Brad Oliver,Aaron Giles,Bernd Wiebelt,Allard van der Bas
#ifndef MAME_VIDEO_VECTOR_H
#define MAME_VIDEO_VECTOR_H

#pragma once

#include "notifier.h"

#include <fstream>
#include <utility>


class vector_device;

class vector_options
{
public:
	friend class vector_device;

	static float s_flicker;
	static float s_beam_width_min;
	static float s_beam_width_max;
	static float s_beam_dot_size;
	static float s_beam_intensity_weight;
	static float s_overscan_x;          // overscan zoom factor about screen centre (1.0 = none)
	static float s_overscan_y;

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
	// Per-line notifier carrying the line endpoints in normalized screen space (0..1 inside the
	// visible area, < 0 or > 1 when off-screen) plus the normalized (0..1) beam energy, for renderers
	// that reproduce off-screen beam effects (e.g. monitor glow). Separate from line_delegate so the
	// existing notifier's signature (part of the Lua API) is left untouched.
	using beam_energy_line_delegate = delegate<void (float, float, float, float, float)>;

	template <typename T> static constexpr rgb_t color111(T c) { return rgb_t(pal1bit(c >> 2), pal1bit(c >> 1), pal1bit(c >> 0)); }
	template <typename T> static constexpr rgb_t color222(T c) { return rgb_t(pal2bit(c >> 4), pal2bit(c >> 2), pal2bit(c >> 0)); }
	template <typename T> static constexpr rgb_t color444(T c) { return rgb_t(pal4bit(c >> 8), pal4bit(c >> 4), pal4bit(c >> 0)); }

	// construction/destruction
	vector_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	uint32_t screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	void clear_list();

	// True when the beam list was not refreshed since the previous frame (the CPU did not start a
	// new list). A renderer can use this to reproduce CRT flicker; the emulation does not act on it.
	bool beam_list_stale() const { return m_beam_list_stale; }

	// True for vector generators whose per-point t0/t1 come from real cycle-accurate sweep timing
	// (the Atari AVG/DVG state machine), so the ELAPSED TIME across a list is physically meaningful -
	// a renderer can use it to reproduce the real "too many vectors for one beam sweep" flicker (a
	// budget-overrun cutoff), which requires timing where t1-t0 truly reflects beam dwell/travel time.
	// False (default) for generators whose t0/t1 do not model that (Vectrex: its own distinct timing
	// the renderer already uses for other purposes; Cinematronics: t0==t1 for most segments).
	void set_avg_timing(bool v) { m_avg_timing = v; }
	bool avg_timing() const { return m_avg_timing; }

	// beam_energy is the normalized (0..1) raw beam energy for renderer overdrive effects.
	// Pass < 0 (the default) when the device has no raw beam-energy signal; the display
	// intensity is then used as the normalized value instead. The displayed intensity is
	// unaffected either way, so non-bgfx output is identical to stock.
	// t0/t1 are the absolute machine times the beam spent drawing this point's line
	// (attotime::never = untimed; only timing-aware vector generators supply them).
	void add_point(int x, int y, rgb_t color, int intensity, float beam_energy = -1.0f,
			attotime t0 = attotime::never, attotime t1 = attotime::never, u32 cap_flags = 0);

	// Optional per-point scale recorded in the -vector_event_dump CSV (driver-specific, e.g. the
	// Vectrex VIA Timer 1 latch = BIOS vector scale). Set just before add_point; -1 = none.
	void set_dump_scale(int s) { m_dump_scale = s; }

	// Optional per-point RAMP-active duration (us) recorded in the dump. Set just before add_point;
	// -1 = none. For Vectrex this is how long RAMP had been active when the point was drawn.
	void set_dump_ramp_us(double r) { m_dump_ramp_us = r; }

	// Optional per-point "curve mid-point" flag recorded in the dump (Vectrex midChange: the beam
	// velocity changed mid-ramp, i.e. a point along an intended curve). Set just before add_point.
	void set_dump_midchange(bool m) { m_dump_midchange = m; }

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

	util::notifier_subscription add_beam_energy_line_notifier(beam_energy_line_delegate &&n);
	template <typename T>
	util::notifier_subscription add_beam_energy_line_notifier(T &&n)
	{ return add_beam_energy_line_notifier(beam_energy_line_delegate(std::forward<T>(n))); }

private:
	float normalized_sigmoid(float n, float k);

	/* The vertices are buffered here */
	struct point
	{
		point() : x(0), y(0), x0(0), y0(0), col(0), intensity(0), beam_energy(0.0f), t0(attotime::never), t1(attotime::never), cap_flags(0), emitted(false) { }

		int x; int y;
		int x0; int y0;     // segment start (previous beam position), so a line re-emits with the correct start
		rgb_t col;
		int intensity;
		float beam_energy;  // normalized (0..1) beam energy passed through to the render primitive
		attotime t0, t1;    // absolute machine time the beam drew this line (never = untimed)
		u32 cap_flags;      // line end-cap terminus bits passed through (bit0 start, bit1 end)
		bool emitted;       // already emitted once: notifiers are not re-fired on a re-drawn stale list
	};

	std::unique_ptr<point[]> m_vector_list;
	int m_vector_index;
	int m_min_intensity;
	int m_max_intensity;
	std::ofstream m_event_dump;
	int m_dump_scale = -1;   // scale value for the next dumped point (set by the driver via set_dump_scale)
	double m_dump_ramp_us = -1.0;   // RAMP-active duration for the next dumped point (set_dump_ramp_us)
	bool m_dump_midchange = false;  // curve mid-point flag for the next dumped point (set_dump_midchange)
	// Generation counters for CRT-flicker detection: clear_list() bumps m_list_generation when the
	// CPU starts a new beam list; screen_update sets m_beam_list_stale when the current frame did not.
	uint32_t m_list_generation;
	uint32_t m_last_drawn_generation;
	bool m_beam_list_stale;
	// render_vector_stats frame counter: increments every screen_update (i.e. every emulated frame
	// the device drew), so a renderer can tell running frames from pause / menu re-presents.
	uint32_t m_stats_frame_id = 0;
	bool m_avg_timing = false;   // set_avg_timing(): this device's t0/t1 model real AVG/DVG sweep time

	// notify interested parties about vector-drawing activities
	util::notifier<> m_frame_begin_notifier;
	util::notifier<> m_frame_end_notifier;
	util::notifier<int, int, uint32_t, int, int> m_move_notifier;
	util::notifier<int, int, int, int, uint32_t, int, int, int> m_line_notifier;
	util::notifier<float, float, float, float, float> m_beam_energy_line_notifier;
};

// device type definition
DECLARE_DEVICE_TYPE(VECTOR, vector_device)

// device iterator
typedef device_type_enumerator<vector_device> vector_device_enumerator;

#endif // MAME_VIDEO_VECTOR_H
