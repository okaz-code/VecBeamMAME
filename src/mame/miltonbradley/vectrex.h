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
		m_io_3dredphase(*this, "3DREDPH"),
		m_io_xskew(*this, "XSKEW"),
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
			attotime t0 = attotime::never, attotime t1 = attotime::never);
	void add_point_stereo(int x, int y, rgb_t color, int intensity, float beam_energy = -1.0f,
			attotime t0 = attotime::never, attotime t1 = attotime::never);
	float calculate_beam_energy(int x0, int y0, int x1, int y1, int intensity, attotime t0, attotime t1) const;

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
	uint8_t m_cb2 = 0;
	void (vectrex_base_state::*vector_add_point_function)(int, int, rgb_t, int, float, attotime, attotime);

	required_device<mc1408_device> m_dac;
	required_device<ay8910_device> m_ay8912;
	required_device<vector_device> m_vector;
	optional_ioport_array<4> m_io_contr;
	required_ioport m_io_buttons;
	required_ioport m_io_3dconf;
	optional_ioport m_io_3dphase;        // 3D imager colour-segment phase trim, coarse (absent on raaspec)
	optional_ioport m_io_3dphase_fine;   // ... fine sub-step trim
	optional_ioport m_io_3dredphase;     // ... red-segment-only phase shift
	optional_ioport m_io_xskew;          // X-axis MUX extra-lag (glyph slant) adjuster
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
