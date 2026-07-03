// license:BSD-3-Clause
// copyright-holders:Mathis Rosenhauer

#ifndef MAME_MILTONBRADLEY_VECTREX_H
#define MAME_MILTONBRADLEY_VECTREX_H

#pragma once

#include "machine/6522via.h"
#include "sound/dac.h"
#include "sound/ay8910.h"
#include "video/vector.h"

#include "bus/vectrex/slot.h"
#include "bus/vectrex/rom.h"

#include "screen.h"

#define NVECT 10000

class vectrex_base_state : public driver_device
{
public:
	void vectrex_cart(device_slot_interface &device);

protected:
	vectrex_base_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_cart(*this, "cartslot"),
		m_via6522_0(*this, "via6522_0"),
		m_gce_vectorram(*this, "gce_vectorram"),
		m_dac(*this, "dac"),
		m_ay8912(*this, "ay8912"),
		m_vector(*this, "vector"),
		m_io_contr(*this, {"CONTR1X", "CONTR1Y", "CONTR2X", "CONTR2Y"}),
		m_io_buttons(*this, "BUTTONS"),
		m_io_3dconf(*this, "3DCONF"),
		m_io_3dphase(*this, "3DPHASE"),
		m_io_3dphase_fine(*this, "3DPHASEF"),
		m_io_xskew(*this, "XSKEW"),
		m_io_beam_infl(*this, "BEAMINFL"),
		m_io_beam_curve(*this, "BEAMCURVE"),
		m_io_beam_max(*this, "BEAMMAX"),
		m_io_dot_ref(*this, "DOTREF"),
		m_io_dot_curve(*this, "DOTCURVE"),
		m_io_dot_max(*this, "DOTMAX"),
		m_io_beam_dwell(*this, "BEAMDWELL"),
		m_io_beam_speed(*this, "BEAMSPEED"),
		m_io_bright(*this, "BRIGHT"),
		m_io_spotkill(*this, "SPOTKILL"),
		m_io_spotkill_ms(*this, "SPOTKMS"),
		m_io_spotkill_range(*this, "SPOTKRNG"),
		m_io_obj_knee(*this, "OBJKNEE"),
		m_io_obj_sharp(*this, "OBJSHARP"),
		m_io_obj_max(*this, "OBJMAX"),
		m_io_obj_star(*this, "OBJSTAR"),
		m_io_lpenconf(*this, "LPENCONF"),
		m_io_lpenx(*this, "LPENX"),
		m_io_lpeny(*this, "LPENY"),
		m_screen(*this, "screen")
	{ }

	void psg_port_w(uint8_t data);
	uint8_t via_r(offs_t offset);
	void via_w(offs_t offset, uint8_t data);
	virtual void driver_start() override;
	virtual void video_start() override ATTR_COLD;
	uint32_t screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect);
	TIMER_CALLBACK_MEMBER(imager_change_color);
	TIMER_CALLBACK_MEMBER(update_level);
	TIMER_CALLBACK_MEMBER(imager_eye);
	TIMER_CALLBACK_MEMBER(imager_index);
	TIMER_CALLBACK_MEMBER(imager_coast);
	TIMER_CALLBACK_MEMBER(lightpen_trigger);
	TIMER_CALLBACK_MEMBER(refresh);
	TIMER_CALLBACK_MEMBER(zero_integrators);
	TIMER_CALLBACK_MEMBER(update_analog);
	TIMER_CALLBACK_MEMBER(update_blank);
	TIMER_CALLBACK_MEMBER(update_mux_enable);
	TIMER_CALLBACK_MEMBER(update_ramp);
	void update_vector();
	uint8_t via_pb_r();
	uint8_t via_pa_r();
	void via_pb_w(uint8_t data);
	void via_pa_w(uint8_t data);
	void via_ca2_w(int state);
	void via_cb2_w(int state);
	void via_irq(int state);

	void vectrex_base(machine_config &config);

	void configure_imager(bool reset_refresh, const double *imager_angles);
	void screen_configuration();
	void multiplexer(int mux);
	void add_point(int x, int y, rgb_t color, int intensity, float beam_energy = -1.0f,
			attotime t0 = attotime::never, attotime t1 = attotime::never, u32 cap_flags = 0);
	void add_point_stereo(int x, int y, rgb_t color, int intensity, float beam_energy = -1.0f,
			attotime t0 = attotime::never, attotime t1 = attotime::never, u32 cap_flags = 0);
	float dot_dwell_energy(int intensity, attotime t0, attotime t1) const;   // parked-beam (dot) dwell-time energy
	float object_boost(int intensity, bool is_point) const;   // per-object-type beam_energy lift (see m_obj_*)
	int apply_brightness(int intensity) const;                // Brightness knob: scale intensity + retrace floor
	float stroke_density_energy(int intensity, double stroke_speed) const;   // stroke-aggregate energy
	void flush_stroke();                                   // emit the buffered RAMP-ON stroke
	float apply_dwell_limit(int x, int y, float energy);   // cap same-location additive pile-up

	unsigned char m_via_out[2];

	required_device<cpu_device> m_maincpu;
	optional_device<vectrex_cart_slot_device> m_cart;

	double m_imager_freq = 0;
	emu_timer *m_imager_color_timers[3]{};
	emu_timer *m_imager_eye_timer = nullptr;
	emu_timer *m_imager_index_timer = nullptr;
	emu_timer *m_imager_coast_timer = nullptr;
	emu_timer *m_imager_level_timer = nullptr;
	emu_timer *m_lp_t = nullptr;

	required_device<via6522_device> m_via6522_0;

private:

	struct vectrex_point
	{
		int x = 0; int y = 0;
		rgb_t col;
		int intensity = 0;
		float beam_energy = -1.0f;
		attotime t0 = attotime::never;
		attotime t1 = attotime::never;
		int scale = 0;   // VIA T1 latch at draw time (BIOS vector scale) - for the event dump
		double ramp_us = 0.0;   // RAMP-active duration up to this point (us); 0 = drawn while RAMP inactive
		bool midchange = false; // true = a curve "mid" point (beam velocity changed mid-ramp); see m_cur_midchange
		u8 cap_flags = 0;       // line end-cap terminus bits: bit0 = RAMP-on start of stroke, bit1 = RAMP-off end
		int eye = 0;   // imager eye this vector was drawn for (0 = none, 1 = left, 2 = right)
	};

	required_shared_ptr<uint8_t> m_gce_vectorram;
	int m_imager_status = 0;
	uint32_t m_beam_color = 0;
	int m_lightpen_port = 0;
	int m_reset_refresh = 0;
	bool m_stereo_sbs = false;   // "3D stereo out = Side-by-side": compress add_point_stereo to half width
	const double *m_imager_angles = nullptr;
	// Cart-detected color disc (angles + refresh strategy), kept so the 3DCONF "3D color disc = Auto"
	// setting can revert to it after a manual override.
	const double *m_cart_imager_angles = nullptr;
	int m_cart_reset_refresh = 0;
	rgb_t m_imager_colors[6];
	unsigned char m_imager_pinlevel = 0;
	int m_old_mcontrol = 0;
	double m_sl = 0;
	double m_pwl = 0;
	int m_x_center = 0;
	int m_y_center = 0;
	int m_x_max = 0;
	int m_y_max = 0;
	int m_x_int = 0;
	int m_y_int = 0;
	int m_lightpen_down = 0;
	int m_pen_x = 0;
	int m_pen_y = 0;
	emu_timer *m_refresh = nullptr;
	emu_timer *m_zero_integrators_timer = nullptr;
	emu_timer *m_update_blank_timer = nullptr;
	emu_timer *m_update_mux_enable_timer = nullptr;
	uint8_t m_blank = 0;
	uint8_t m_ramp = 0;
	int8_t m_analog[5]{};
	int m_point_index = 0;
	int m_display_start = 0;
	int m_display_end = 0;
	vectrex_point m_points[NVECT];
	// 3D imager "Separate images" per-eye frame retention (layer B / eye-tag). Each eye's last completed
	// frame is COPIED out (not a range into the wrapping ring) and capped, so screen_update can redraw
	// both eyes stably without flooding the renderer when the wheel is slow (long eye intervals -> many
	// points / ring wrap). 1 = left, 2 = right.
	static constexpr int EYE_FRAME_MAX = 2048;
	vectrex_point m_eye_frame[3][EYE_FRAME_MAX];
	int m_eye_count[3] = { 0, 0, 0 };
	int m_eye_draw_start = 0;   // m_point_index where the current eye's drawing began
	uint16_t m_via_timer2 = 0;
	attotime m_vector_start_time;
		int m_cur_scale = 0;   // VIA T1 latch sampled at update_vector time (BIOS draw scale)
	attotime m_ramp_start_time;   // time RAMP last went active; ramp_us = now - this while RAMP active
	double m_cur_ramp_us = 0.0;   // RAMP-active duration sampled in update_vector, copied into each point
	// beam_energy draw-time model params, cached once per frame in screen_configuration()
	double m_beam_infl = 0.5;        // influence (0..1): 0 = flat intensity, 1 = fully draw-time shaped
	double m_beam_curve = 1.0;       // draw-time saturation exponent g (gentleness)
	double m_beam_max = 4.0;         // per-unit-area max beam_energy (phosphor saturation ceiling)
	// Parked-dot dwell model params (dot_dwell_energy): real-hw calibrated - the dazzle onset of a
	// dwelling dot sits on an I x dt contour, so the response must be near-linear over ~30..200us.
	double m_dot_ref = 250e-6;       // dwell normalizer T_ref (seconds): x = dt / T_ref
	double m_dot_curve = 1.0;        // dwell saturation exponent (1.0 = the I x dt product region is linear)
	double m_dot_max = 12.8;         // parked-dot energy ceiling (independent of the line/stroke m_beam_max)
	// Same-location dwell accumulation limit: caps the cumulative beam_energy deposited at one parked
	// spot (the additive pile-up of repeated dots). First dot at a new spot is always full.
	double m_dwell_cap = 8.0;        // cached per frame (0 = only the first dot, large = unlimited)
	int m_dwell_x = 0;
	int m_dwell_y = 0;
	double m_dwell_accum = 0.0;
	// midChange detection: within a run of connected lit RAMP-active segments, a vertex is a "curve
	// point" when the beam velocity changed there. Used to reconstruct intended curves (Catmull-Rom).
	int m_mid_prev_dx = 0;
	int m_mid_prev_dy = 0;
	bool m_mid_in_run = false;    // currently inside a connected lit ramp run
	bool m_cur_midchange = false; // midChange flag for the next add_point
	// Stroke-aggregate energy model: a RAMP-ON stroke is collected, then at RAMP-off the whole stroke's
	// speed (total moved distance / RAMP time) sets ONE brightness density applied to every visible
	// sub-segment, so BLANK/SR act as a pure visibility mask. This stabilises BIOS raster text, where a
	// per-segment dt model would make each tiny SR-gated dot dim and jittery. Cached per frame.
	struct stroke_seg
	{
		int x0, y0, x1, y1;      // segment endpoints
		attotime t0, t1;
		int intensity;           // 0 = blank (invisible move); >0 = visible
		rgb_t col;
		bool midchange;
		int scale;
		double ramp_us;
	};
	std::vector<stroke_seg> m_stroke;   // current RAMP-ON stroke, flushed at RAMP-off / ZERO / refresh
	double m_beam_speed = 100.0;        // cached BEAMSPEED inverse-speed normalizer
	// Brightness knob (BRIGHT): master intensity gain. 50 = x1 (normal), 100 = x2. At high brightness the
	// blanked-beam retrace is lifted to m_bright_floor so it faintly glows, like a real CRT.
	float m_bright_mult = 1.0f;         // cached intensity multiplier
	int m_bright_floor = 0;             // cached intensity floor for blanked (intensity-0) beams (0 = none)
	// Spot killer (deflection protection): if the beam stops moving for m_spotkill_secs the beam is cut
	// (screen goes black) to model the real Vectrex CRT-burn protection. m_last_move_time = last deflection.
	bool m_spotkill = false;            // cached SPOTKILL enable
	double m_spotkill_secs = 0.25;      // cached no-deflection time before the beam is cut
	int m_spotkill_rx = 0;              // cached kill radius from centre (X, 16.16 units): beam beyond = active
	int m_spotkill_ry = 0;              // cached kill radius from centre (Y)
	attotime m_last_move_time;          // last time the beam swung beyond the kill radius (deflection active)
	// Object-type brightness/width lift: beam_energy *= smooth curve of intensity (Z). Below the knee the
	// factor is ~1 (normal: enemies/ship); past the adjustable knee it rises toward m_obj_max (very
	// prominent: bullets/explosions). Parked dots (stars) get an extra m_obj_star. Cached per frame.
	float m_obj_knee = 1.0f;            // intensity (0..1) where the lift starts; 1.0 = never (off)
	float m_obj_sharp = 2.0f;           // curve sharpness past the knee
	float m_obj_max = 1.0f;             // max multiplier at full intensity (1.0 = off)
	float m_obj_star = 1.0f;            // extra multiplier for length-0 parked dots (1.0 = off)
	uint8_t m_cb2 = 0;
	void (vectrex_base_state::*vector_add_point_function)(int, int, rgb_t, int, float, attotime, attotime, u32);

	required_device<mc1408_device> m_dac;
	required_device<ay8910_device> m_ay8912;
	required_device<vector_device> m_vector;
	optional_ioport_array<4> m_io_contr;
	required_ioport m_io_buttons;
	required_ioport m_io_3dconf;
	optional_ioport m_io_3dphase;        // 3D imager colour-segment phase trim, coarse (absent on raaspec)
	optional_ioport m_io_3dphase_fine;   // ... fine sub-step trim
	optional_ioport m_io_xskew;          // X-axis MUX extra-lag (glyph slant) adjuster
	optional_ioport m_io_beam_infl;      // beam_energy draw-time influence (0..100): flat intensity -> draw-time shaped
	optional_ioport m_io_beam_curve;     // ... draw-time (dt) saturation curve / gentleness
	optional_ioport m_io_beam_max;       // ... per-unit-area max beam_energy (phosphor saturation ceiling)
	optional_ioport m_io_dot_ref;        // parked-dot dwell normalizer T_ref (adj x 5us)
	optional_ioport m_io_dot_curve;      // parked-dot dwell saturation exponent (adj/50)
	optional_ioport m_io_dot_max;        // parked-dot energy ceiling (adj/100 x 16)
	optional_ioport m_io_beam_dwell;     // same-location accumulation limit (parked-beam pile-up cap)
	optional_ioport m_io_beam_speed;     // stroke inverse-speed normalizer
	optional_ioport m_io_bright;         // Brightness knob: 50 = normal (x1), 100 = x2 intensity
	optional_ioport m_io_spotkill;       // spot killer: cut the beam when deflection stops (CRT burn protection)
	optional_ioport m_io_spotkill_ms;    // spot killer time constant (no-deflection time before the beam is cut)
	optional_ioport m_io_spotkill_range; // spot killer trigger range: 0 = centre only, 100 = full draw range
	optional_ioport m_io_obj_knee;       // object lift: intensity (Z) where the brightness/width lift begins
	optional_ioport m_io_obj_sharp;      // object lift: curve sharpness past the knee
	optional_ioport m_io_obj_max;        // object lift: max multiplier at full intensity (1.0 = off)
	optional_ioport m_io_obj_star;       // object lift: extra multiplier for parked dots (stars); 1.0 = off
	required_ioport m_io_lpenconf;
	required_ioport m_io_lpenx;
	required_ioport m_io_lpeny;
	required_device<screen_device> m_screen;
};


class vectrex_state : public vectrex_base_state
{
public:
	vectrex_state(const machine_config &mconfig, device_type type, const char *tag) :
		vectrex_base_state(mconfig, type, tag)
	{ }

	void vectrex(machine_config &config);

protected:
	virtual void video_start() override ATTR_COLD;
	virtual void machine_start() override ATTR_COLD;

private:
	void vectrex_map(address_map &map) ATTR_COLD;
};


class raaspec_state : public vectrex_base_state
{
public:
	raaspec_state(const machine_config &mconfig, device_type type, const char *tag) :
		vectrex_base_state(mconfig, type, tag),
		m_io_coin(*this, "COIN")
	{ }

	void raaspec(machine_config &config);

private:
	void raaspec_led_w(uint8_t data);
	uint8_t s1_via_pb_r();

	void raaspec_map(address_map &map) ATTR_COLD;

	required_ioport m_io_coin;
};

#endif // MAME_MILTONBRADLEY_VECTREX_H
