// license:BSD-3-Clause
// copyright-holders:Mathis Rosenhauer

#include "emu.h"
#include "vectrex.h"
#include "cpu/m6809/m6809.h"

#include <algorithm>
#include <cmath>
#include <numbers>


#define ANALOG_DELAY 8500

// Italic/slant reproduction: extra delay (ns) applied to the Y integrator's DAC application (in
// via_pa_w), subtracted from ANALOG_DELAY, so Y leads the MUX-routed X axis. Portrait screen (rotated left
// 90deg), so the shear reads as the italic slant along Y. This per-axis timing asymmetry shears the
// characteristic Vectrex slant that vectrexy reproduces via its VelocityXDelay (~6 CPU cycles
// @1.5MHz ~= 4us). Live-adjustable via the "XSKEW" PORT_ADJUSTER (0..100, default 50): the delay is
// adjuster * X_SKEW_NS_PER_STEP, so 50 -> 4000ns, 100 -> 8000ns, 0 -> upright. Tune to real HW / vectrexy.
#define X_SKEW_NS_PER_STEP 80

#define INT_PER_CLOCK 550
#define BEAM_ENERGY_NORMALIZE 480000.0

/*********************************************************************

   Enums and typedefs

*********************************************************************/

enum {
	PORTB = 0,
	PORTA
};

enum {
	A_X = 0,
	A_ZR,
	A_Z,
	A_AUDIO,
	A_Y
};

/*********************************************************************

   Lightpen

*********************************************************************/

TIMER_CALLBACK_MEMBER(vectrex_base_state::lightpen_trigger)
{
	if (m_lightpen_port & 1)
	{
		m_via6522_0->write_ca1(1);
		m_via6522_0->write_ca1(0);
	}

	if (m_lightpen_port & 2)
	{
		m_maincpu->pulse_input_line(M6809_FIRQ_LINE, m_maincpu->minimum_quantum_time());
	}
}


/*********************************************************************

   VIA T2 configuration

   We need to snoop the frequency of VIA timer 2 here since most
   vectrex games use that timer for steady screen refresh. Even if the
   game stops T2 we continue refreshing the screen with the last known
   frequency. Note that we quickly get out of sync in this case and the
   screen will start flickering (see cut scenes in Spike).

   Note that the timer can be adjusted to the full period each time T2
   is restarted. This behaviour avoids flicker in most games. Some
   games like mine 3d don't work well with this scheme though and show
   severe jerking. So the second option is to leave the current period
   alone (if the new period isn't shorter) and change only the next
   full period.

*********************************************************************/

uint8_t vectrex_base_state::via_r(offs_t offset)
{
	return m_via6522_0->read(offset);
}

void vectrex_base_state::via_w(offs_t offset, uint8_t data)
{
	attotime period;

	switch (offset)
	{
	case 8:
		m_via_timer2 = (m_via_timer2 & 0xff00) | data;
		break;

	case 9:
		m_via_timer2 = (m_via_timer2 & 0x00ff) | (data << 8);

		period = m_maincpu->cycles_to_attotime(m_via_timer2);

		if (m_reset_refresh)
			m_refresh->adjust(period, 0, period);
		else
			m_refresh->adjust(std::min(period, m_refresh->remaining()), 0, period);
		break;
	}
	m_via6522_0->write(offset, data);
}


/*********************************************************************

   Screen refresh

*********************************************************************/

TIMER_CALLBACK_MEMBER(vectrex_base_state::refresh)
{
	/* Refresh only marks the range of vectors which will be drawn
	 * during the next screen_update. */
	m_display_start = m_display_end;
	m_display_end = m_point_index;
}


uint32_t vectrex_base_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	screen_configuration();

	// 3D imager stereo output. Both eyes' last completed frames (layer B per-eye retention) are redrawn
	// together every refresh so the pair is stable (no left/right flicker). The 3DCONF "3D stereo out"
	// field picks how to combine them:
	//   0x000 off  : legacy behaviour (overlay, or the "Separate images" 90deg side-by-side split per 0x02)
	//   0x400 SBS  : the same 90deg side-by-side split but width-compressed to a proper half each (the
	//                compression is baked at draw time by add_point_stereo via m_stereo_sbs)
	//   0x800 ana  : colour anaglyph - left eye -> red channel, right eye -> cyan, overlaid (uses the
	//                overlay coordinates, so screen_configuration forces add_point and Separate is off)
	// All paths fall back to the stock single-range draw when the imager is off / the wheel is not spinning
	// / no frames captured yet (the per-eye buffers are capped at EYE_FRAME_MAX so the renderer is never
	// flooded). The Vectrex screen is portrait, so the side-by-side pair appears stacked until a 90deg
	// Video Rotate puts the two eyes left/right.
	const ioport_value conf = m_io_3dconf->read();
	const ioport_value stereo_mode = conf & 0xC00;
	const bool have_eyes = (conf & 0x01) && m_imager_freq > 5.0 && m_eye_count[1] > 0 && m_eye_count[2] > 0;

	if (have_eyes && stereo_mode == 0x800)   // colour anaglyph (left = red, right = cyan; swap optional)
	{
		const bool swap = (conf & 0x1000) != 0;   // On -> left = cyan, right = red
		for (int e = 1; e <= 2; e++)
		{
			// eye 1 = left -> red, eye 2 = right -> cyan by default; the swap flips which eye is red.
			const bool red_eye = ((e == 1) != swap);
			auto ana = [red_eye](rgb_t c) -> rgb_t { return red_eye ? rgb_t(c.r(), 0, 0) : rgb_t(0, c.g(), c.b()); };
			m_vector->add_point(m_eye_frame[e][0].x, m_eye_frame[e][0].y, ana(m_eye_frame[e][0].col), 0);
			for (int i = 0; i < m_eye_count[e]; i++)
				m_vector->add_point(m_eye_frame[e][i].x, m_eye_frame[e][i].y, ana(m_eye_frame[e][i].col), m_eye_frame[e][i].intensity);
		}
	}
	else if (have_eyes && (stereo_mode == 0x400 || (conf & 0x02)))   // side-by-side / "Separate images"
	{
		// Both eyes' last completed frames, replayed stable (the coordinates - including the SBS half-width
		// compression for stereo_mode 0x400 - are already baked by add_point_stereo at draw time).
		for (int e = 1; e <= 2; e++)
		{
			m_vector->add_point(m_eye_frame[e][0].x, m_eye_frame[e][0].y, m_eye_frame[e][0].col, 0);
			for (int i = 0; i < m_eye_count[e]; i++)
				m_vector->add_point(m_eye_frame[e][i].x, m_eye_frame[e][i].y, m_eye_frame[e][i].col, m_eye_frame[e][i].intensity);
		}
	}
	else
	{
		/* start black */
		m_vector->add_point(m_points[m_display_start].x,
							m_points[m_display_start].y,
							m_points[m_display_start].col,
							0);

		for (int i = m_display_start; i != m_display_end; i = (i + 1) % NVECT)
		{
			m_vector->set_dump_scale(m_points[i].scale);   // BIOS draw scale -> event dump
			m_vector->add_point(m_points[i].x,
								m_points[i].y,
								m_points[i].col,
								m_points[i].intensity,
								m_points[i].beam_energy,
								m_points[i].t0,
								m_points[i].t1);
		}
	}

	m_vector->screen_update(screen, bitmap, cliprect);
	m_vector->clear_list();
	return 0;
}


/*********************************************************************

   Vector functions

*********************************************************************/

void vectrex_base_state::add_point(int x, int y, rgb_t color, int intensity, float beam_energy, attotime t0, attotime t1)
{
	vectrex_point *newpoint;

	m_point_index = (m_point_index+1) % NVECT;
	newpoint = &m_points[m_point_index];

	newpoint->x = x;
	newpoint->y = y;
	newpoint->col = color;
	newpoint->intensity = intensity;
	newpoint->beam_energy = beam_energy;
	newpoint->t0 = t0;
	newpoint->t1 = t1;
	newpoint->scale = m_cur_scale;     // BIOS vector scale (VIA T1 latch) sampled in update_vector
	newpoint->eye = m_imager_status;   // tag with the eye currently being drawn (0 = imager off)
}


void vectrex_base_state::add_point_stereo(int x, int y, rgb_t color, int intensity, float beam_energy, attotime t0, attotime t1)
{
	constexpr double SQRT1_2 = std::numbers::sqrt2 / 2.0;

	// The 90-degree side-by-side split that fits the two eye images into the portrait screen (left in
	// [0, x_center), right offset by x_center). "3D stereo out = Side-by-side" additionally compresses the
	// width axis (the L/R separation, x here) to half so each eye occupies a proper half - standard SBS.
	// ctr re-centres the compressed image within its half (otherwise it sits at the left edge): map the
	// content centre (y = m_y_center) to the centre of the half (x_center/2).
	const double xs = m_stereo_sbs ? (SQRT1_2 * 0.5) : SQRT1_2;
	const int ctr = m_stereo_sbs ? (m_x_center / 2 - (int)(m_y_center * xs)) : 0;

	// eye 1 (left) -> right screen half, eye 2 (right) -> left half: the corrected stereo orientation
	// (the un-swapped mapping showed the eyes reversed).
	const bool to_right = (m_imager_status != 2);   /* left = 1, right = 2 */
	add_point((int)(y * xs) + (to_right ? m_x_center : 0) + ctr,
			(int)((m_x_max - x) * SQRT1_2), color, intensity, beam_energy, t0, t1);
}


float vectrex_base_state::calculate_beam_energy(int x0, int y0, int x1, int y1, int intensity, attotime t0, attotime t1) const
{
	if (t0.is_never() || t1.is_never() || t1 <= t0 || intensity <= 0)
		return -1.0f;

	const double dt = (t1 - t0).as_double();
	const double beam_current = std::clamp(double(intensity) / 255.0, 0.0, 1.0);
	const double dx = double(x1) - double(x0);
	const double dy = double(y1) - double(y0);
	const double length = std::hypot(dx, dy) / 65536.0;

	double energy = 0.0;
	if (length < 0.01)
		energy = beam_current * dt * BEAM_ENERGY_NORMALIZE;
	else
		energy = beam_current * dt * BEAM_ENERGY_NORMALIZE / length;

	// No upper clamp at 1.0: a dwelling beam (parked point, length~0) physically deposits far more
	// energy than a swept line, so beam_energy is allowed to exceed 1.0 (capped at 8x peak). The
	// renderer clamps it to 0..1 for the displayed core but drives the overdrive/HDR white-hot flare
	// from the raw value, so dwell points blow out far brighter than peak lines (real Vectrex behaviour).
	return float(std::clamp(energy, 0.0, 16.0));
}


TIMER_CALLBACK_MEMBER(vectrex_base_state::zero_integrators)
{
	m_x_int = m_x_center + (m_analog[A_ZR] * INT_PER_CLOCK);
	m_y_int = m_y_center + (m_analog[A_ZR] * INT_PER_CLOCK);
	(this->*vector_add_point_function)(m_x_int, m_y_int, m_beam_color, 0, -1.0f, attotime::never, attotime::never);
}


/*********************************************************************

   Delayed signals

   The RAMP signal is delayed wrt. beam blanking. Getting this right
   is important for text placement, the maze in Clean Sweep and many
   other games.

*********************************************************************/

void vectrex_base_state::update_vector()
{
	int length;
	const attotime t0 = m_vector_start_time;
	const attotime t1 = machine().time();
	const int x0 = m_x_int;
	const int y0 = m_y_int;
	m_cur_scale = m_via6522_0->t1_latch();   // BIOS vector scale (VIA Timer 1 latch) at draw time

	if (!m_ramp)
	{
		length = m_maincpu->clocks_to_cycles(m_maincpu->clock()) * INT_PER_CLOCK
			* (t1 - t0).as_double();

		m_x_int += length * (m_analog[A_X] + m_analog[A_ZR]);
		m_y_int += length * (m_analog[A_Y] + m_analog[A_ZR]);

		const int intensity = 2 * m_analog[A_Z] * m_blank;
		(this->*vector_add_point_function)(m_x_int, m_y_int, m_beam_color, intensity,
				calculate_beam_energy(x0, y0, m_x_int, m_y_int, intensity, t0, t1), t0, t1);
	}
	else
	{
		if (m_blank)
		{
			const int intensity = 2 * m_analog[A_Z];
			(this->*vector_add_point_function)(m_x_int, m_y_int, m_beam_color, intensity,
					calculate_beam_energy(x0, y0, m_x_int, m_y_int, intensity, t0, t1), t0, t1);
		}
	}

	m_vector_start_time = t1;
}


/*********************************************************************

   Startup

*********************************************************************/

void vectrex_base_state::video_start()
{
	const rectangle &visarea = m_screen->visible_area();

	m_x_center=(visarea.width() / 2) << 16;
	m_y_center=(visarea.height() / 2) << 16;
	m_x_max = visarea.max_x << 16;
	m_y_max = visarea.max_y << 16;

	vector_add_point_function = &vectrex_base_state::add_point;

	m_refresh = timer_alloc(FUNC(vectrex_base_state::refresh), this);
	m_zero_integrators_timer = timer_alloc(FUNC(vectrex_base_state::zero_integrators), this);
	m_update_blank_timer = timer_alloc(FUNC(vectrex_base_state::update_blank), this);
	m_update_mux_enable_timer = timer_alloc(FUNC(vectrex_base_state::update_mux_enable), this);

	m_display_start = 0;
	m_display_end = 0;
	m_reset_refresh = 0;
	m_blank = 0;
	m_ramp = 0;
	std::fill(std::begin(m_analog), std::end(m_analog), 0);
	m_point_index = 0;
	m_lightpen_down = 0;
}

void vectrex_state::video_start()
{
	vectrex_base_state::video_start();

	m_imager_freq = 1;

	m_imager_eye_timer = timer_alloc(FUNC(vectrex_state::imager_eye), this);
	m_imager_index_timer = timer_alloc(FUNC(vectrex_state::imager_index), this);
	m_imager_index_timer->adjust(attotime::from_hz(m_imager_freq), 2, attotime::from_hz(m_imager_freq));

	// Coast timer (problem 3): the colour wheel's speed is only integrated on PWM edges in psg_port_w, so
	// once the game stops driving the motor the wheel never spins down. Poll periodically and apply friction
	// when the motor is idle, halting the wheel timers when it stops.
	m_imager_coast_timer = timer_alloc(FUNC(vectrex_state::imager_coast), this);
	m_imager_coast_timer->adjust(attotime::from_msec(50), 0, attotime::from_msec(50));

	for (int i = 0; i < 3; i++)
	{
		m_imager_color_timers[i] = timer_alloc(FUNC(vectrex_state::imager_change_color), this);
		m_imager_color_timers[i]->adjust(attotime::never);
	}

	m_imager_level_timer = timer_alloc(FUNC(vectrex_state::update_level), this);

	m_lp_t = timer_alloc(FUNC(vectrex_state::lightpen_trigger), this);
}


/*********************************************************************

   VIA interface functions

*********************************************************************/

void vectrex_base_state::multiplexer(int mux)
{
	// MUX-routed channels (A_X / A_Z / A_ZR / A_AUDIO) settle at ANALOG_DELAY. The glyph-slant skew is
	// applied to the directly-driven Y axis in via_pa_w (see X_SKEW_NS_PER_STEP), not here.
	machine().scheduler().timer_set(attotime::from_nsec(ANALOG_DELAY), timer_expired_delegate(FUNC(vectrex_base_state::update_analog), this), m_via_out[PORTA] << 9 | 0x100 | mux);

	if (mux == A_AUDIO)
		m_dac->write(m_via_out[PORTA] ^ 0x80); // not gate shown on schematic
}


void vectrex_base_state::via_pb_w(uint8_t data)
{
	if (!(data & 0x80))
	{
		/* RAMP is active */
		if ((m_ramp & 0x80))
		{
			/* RAMP was inactive before */

			if (m_lightpen_down)
			{
				/* Simple lin. algebra to check if pen is near
				 * the line defined by (A_X,A_Y).
				 * If that is the case, set a timer which goes
				 * off when the beam reaches the pen. Exact
				 * timing is important here.
				 *
				 *    lightpen
				 *       ^
				 *  _   /|
				 *  b  / |
				 *    /  |
				 *   /   |d
				 *  /    |
				 * /     |
				 * ------+---------> beam path
				 *    l  |    _
				 *            a
				 */
				double a2, b2, ab, d2;
				ab = (m_pen_x - m_x_int) * m_analog[A_X]
					+(m_pen_y - m_y_int) * m_analog[A_Y];
				if (ab > 0)
				{
					a2 = (double)(m_analog[A_X] * m_analog[A_X]
									+(double)m_analog[A_Y] * m_analog[A_Y]);
					b2 = (double)(m_pen_x - m_x_int) * (m_pen_x - m_x_int)
						+(double)(m_pen_y - m_y_int) * (m_pen_y - m_y_int);
					d2 = b2 - ab * ab / a2;
					if (d2 < 2e10 && m_analog[A_Z] * m_blank > 0)
						m_lp_t->adjust(attotime::from_double(ab / a2 / (m_maincpu->clocks_to_cycles(m_maincpu->clock()) * INT_PER_CLOCK)));
				}
			}
		}

		if (!(data & 0x1) && (m_via_out[PORTB] & 0x1))
		{
			/* MUX has been enabled */
			m_update_mux_enable_timer->adjust(attotime::from_nsec(ANALOG_DELAY));
		}
	}
	else
	{
		/* RAMP is inactive */
		if (!(m_ramp & 0x80))
		{
			/* Cancel running timer, line already finished */
			if (m_lightpen_down)
				m_lp_t->adjust(attotime::never);
		}
	}

	/* Cartridge bank-switching */
	if (m_cart && ((data ^ m_via_out[PORTB]) & 0x40))
		m_cart->write_bank(data);

	/* Sound */
	if (data & 0x10)
	{
		if (data & 0x08) /* BC1 (do we select a reg or write it ?) */
			m_ay8912->address_w(m_via_out[PORTA]);
		else
			m_ay8912->data_w(m_via_out[PORTA]);
	}

	if (!(data & 0x1) && (m_via_out[PORTB] & 0x1))
		multiplexer((data >> 1) & 0x3);

	m_via_out[PORTB] = data;
	machine().scheduler().timer_set(attotime::from_nsec(ANALOG_DELAY), timer_expired_delegate(FUNC(vectrex_base_state::update_ramp), this), data & 0x80);
}


void vectrex_base_state::via_pa_w(uint8_t data)
{
	/* DAC output always goes to Y integrator */
	m_via_out[PORTA] = data;
	// Glyph-slant skew: advance the directly-driven Y integrator AHEAD of the MUX-routed X axis (Y
	// settles earlier). The screen is portrait (rotated left 90deg) so the italic shear reads along Y.
	// XSKEW adjuster * step ns subtracted from ANALOG_DELAY; 0 = stock upright. (Y is applied here, not
	// via multiplexer(), so the skew must live here.)
	const int skew = int(m_io_xskew.read_safe(0)) * X_SKEW_NS_PER_STEP;
	machine().scheduler().timer_set(attotime::from_nsec(ANALOG_DELAY - skew), timer_expired_delegate(FUNC(vectrex_base_state::update_analog), this), A_Y);

	if (!(m_via_out[PORTB] & 0x1))
		multiplexer((m_via_out[PORTB] >> 1) & 0x3);
}


void vectrex_base_state::via_ca2_w(int state)
{
	if (state == 0)
		m_zero_integrators_timer->adjust(attotime::from_nsec(ANALOG_DELAY));
}


void vectrex_base_state::via_cb2_w(int state)
{
	if (m_cb2 != state)
	{
		/* Check lightpen */
		if (m_lightpen_port != 0)
		{
			m_lightpen_down = ioport("LPENCONF")->read() & 0x10;

			if (m_lightpen_down)
			{
				m_pen_x = ioport("LPENX")->read() * (m_x_max / 0xff);
				m_pen_y = ioport("LPENY")->read() * (m_y_max / 0xff);

				int dx = abs(m_pen_x - m_x_int);
				int dy = abs(m_pen_y - m_y_int);
				if (dx < 500000 && dy < 500000 && state > 0)
					m_lp_t->adjust(attotime::zero);
			}
		}

		m_update_blank_timer->adjust(attotime::zero, state);
		m_cb2 = state;
	}
}


/*****************************************************************

   RA+A Spectrum I+

*****************************************************************/

void raaspec_state::raaspec_led_w(uint8_t data)
{
	logerror("Spectrum I+ LED: %i%i%i%i%i%i%i%i\n",
				(data>>7)&0x1, (data>>6)&0x1, (data>>5)&0x1, (data>>4)&0x1,
				(data>>3)&0x1, (data>>2)&0x1, (data>>1)&0x1, data&0x1);
}
