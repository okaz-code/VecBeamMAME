// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    Cinematronics vector hardware

***************************************************************************/

#include "emu.h"
#include "cinemat.h"


/*************************************
 *
 *  Vector rendering
 *
 *************************************/

void cinemat_state::cinemat_vector_callback(int16_t sx, int16_t sy, int16_t ex, int16_t ey, uint8_t shift)
{
	const rectangle &visarea = m_screen->visible_area();

	// Beam timing: stamp each vector with the machine time it is drawn (this callback runs during CCPU
	// execution). The CCPU redraw is VBLANK-locked, so the timestamp is frame-grained. The t0/t1 span
	// carries per-segment beam timing (dwell) used by the renderer's energy model and event dump.
	const attotime now = machine().time();

	/* adjust for slop */
	sx -= visarea.left();
	ex -= visarea.left();
	sy -= visarea.top();
	ey -= visarea.top();

	// Sweep time: a fixed per-DV overhead plus the time the beam needs to cover the length. A
	// Cinematronics monitor has no Z input - brightness IS sweep speed - so this is the whole of the
	// game's control over how bright a stroke comes out, and the renderer's energy model reads it
	// straight off the t0/t1 span.
	//
	// Both terms are needed. Without the length term every vector sweeps for the same time, so a long
	// one is drawn proportionally faster and fades out: measured on QB-3, a 200px stroke came out at
	// 0.09 against 1.16 for a 20px one. Without the overhead a degenerate vector takes no time at all
	// and there is nothing to make a parked dot bright.
	//
	// It used to be neither: only sx==ex && sy==ey was given a span, synthesised from the CCPU timer
	// as shift * 30us, and every real stroke went out with t0 == t1, which the renderer reads as "no
	// timing" and leaves at the plain intensity. That put a cliff at zero length - 3.06 against 1.00
	// on QB-3 - and the dots bloomed into blobs several times the width of the strokes they belonged
	// to. The timer needs no brightness term of its own either: the CPU core already draws
	// delta >> T, so the timer sets the LENGTH, and the length now sets the speed.
	//
	// Both figures are calibrations, not measurements - MAME does not model the CCPU's sweep, and the
	// survey that catalogued these engines lists this one as needing a model added. The overhead is
	// set where the renderer's two energy branches meet: a vanishing line's speed tends to zero, so
	// the line branch tends to its own ceiling (energy_line_max), while a parked dot goes through the
	// dot branch (energy_dot_ref, energy_dot_max), and with the shipped chain values the two agree at
	// about 24us. The rate puts a long stroke near the chain's speed reference so it neither saturates
	// nor fades. Together they give QB-3 a frame of 26.9ms against its 26.3ms period, i.e. a beam that
	// is busy nearly all the time, which is what a machine that flickers when the scene gets busy
	// should look like.
	// -vector_ccpu_dwell / -vector_ccpu_rate, cached in machine_start.
	const double dv_len = sqrt(double(ex - sx) * double(ex - sx) + double(ey - sy) * double(ey - sy));
	const double dv_us = m_dv_dwell_us + dv_len / std::max(0.01, m_dv_rate_px_us);
	const attotime t1 = now + attotime::from_usec(int(dv_us + 0.5));

	// Display intensity. The stock code turned the timer into brightness as 0x1ff * shift / 8, which
	// overflows the 8-bit field for shift > 3 (shift 5 -> 319 -> wraps to 63). The timer no longer
	// carries brightness - it sets the length, and the length sets the speed - so this is just the
	// clamp that stops the wrap.
	int intensity = (sx == ex && sy == ey) ? std::min(0x1ff * shift / 8, 0xff) : 0xff;

	/* move to the starting position if we're not there already */
	if (sx != m_lastx || sy != m_lasty)
		m_vector->add_point(sx << 16, sy << 16, 0, 0, -1.0f, now, now);

	/* draw the vector */
	m_vector->add_point(ex << 16, ey << 16, m_vector_color, intensity, -1.0f, now, t1);

	/* remember the last point */
	m_lastx = ex;
	m_lasty = ey;
}



/*************************************
 *
 *  Vector color handling
 *
 *************************************/

void cinemat_state::vector_control_w(int state)
{
	/* color is either bright or dim, selected by the value sent to the port */
	m_vector_color = state ? rgb_t(0x80,0x80,0x80) : rgb_t(0xff,0xff,0xff);
}


void cinemat_16level_state::vector_control_w(int state)
{
	/* on the rising edge of the data value, latch bits 0-3 of the */
	/* X register as the intensity */
	if (state)
	{
		int xval = m_maincpu->state_int(ccpu_cpu_device::CCPU_X) & 0x0f;
		int i = (xval + 1) * 255 / 16;
		m_vector_color = rgb_t(i,i,i);
	}
}


void cinemat_64level_state::vector_control_w(int state)
{
	/* on the rising edge of the data value, latch bits 2-7 of the */
	/* X register as the intensity */
	if (state)
	{
		int xval = m_maincpu->state_int(ccpu_cpu_device::CCPU_X);
		xval = (~xval >> 2) & 0x3f;
		int i = (xval + 1) * 255 / 64;
		m_vector_color = rgb_t(i, i, i);
	}
}


void cinemat_color_state::vector_control_w(int state)
{
	/* on the rising edge of the data value, latch the X register */
	/* as 4-4-4 BGR values */
	if (state)
	{
		int xval = m_maincpu->state_int(ccpu_cpu_device::CCPU_X);
		int r = (~xval >> 0) & 0x0f;
		r = r * 255 / 15;
		int g = (~xval >> 4) & 0x0f;
		g = g * 255 / 15;
		int b = (~xval >> 8) & 0x0f;
		b = b * 255 / 15;
		m_vector_color = rgb_t(r,g,b);
	}
}


void qb3_state::vector_control_w(int state)
{
	/* on the falling edge of the data value, remember the original X,Y values */
	/* they will be restored on the rising edge; this is to simulate the fact */
	/* that the Rockola color hardware did not overwrite the beam X,Y position */
	/* on an IV instruction if data == 0 here */
	if (!state)
	{
		m_qb3_lastx = m_maincpu->state_int(ccpu_cpu_device::CCPU_X);
		m_qb3_lasty = m_maincpu->state_int(ccpu_cpu_device::CCPU_Y);
	}

	/* on the rising edge of the data value, latch the Y register */
	/* as 2-3-3 BGR values */
	if (state)
	{
		int yval = m_maincpu->state_int(ccpu_cpu_device::CCPU_Y);
		int r = (~yval >> 0) & 0x07;
		r = r * 255 / 7;
		int g = (~yval >> 3) & 0x07;
		g = g * 255 / 7;
		int b = (~yval >> 6) & 0x03;
		b = b * 255 / 3;
		m_vector_color = rgb_t(r,g,b);

		/* restore the original X,Y values */
		m_maincpu->set_state_int(ccpu_cpu_device::CCPU_X, m_qb3_lastx);
		m_maincpu->set_state_int(ccpu_cpu_device::CCPU_Y, m_qb3_lasty);
	}
}



/*************************************
 *
 *  End-of-frame
 *
 *************************************/

uint32_t cinemat_state::screen_update_cinemat(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	m_vector->screen_update(screen, bitmap, cliprect);
	m_vector->clear_list();

	return 0;
}



/*************************************
 *
 *  Space War update
 *
 *************************************/

uint32_t cinemat_state::screen_update_spacewar(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	int sw_option = ~m_inputs->read();

	screen_update_cinemat(screen, bitmap, cliprect);

	/* set the state of the artwork */
	m_pressed[3] = BIT(sw_option, 0);
	m_pressed[8] = BIT(sw_option, 1);
	m_pressed[4] = BIT(sw_option, 2);
	m_pressed[9] = BIT(sw_option, 3);
	m_pressed[1] = BIT(sw_option, 4);
	m_pressed[6] = BIT(sw_option, 5);
	m_pressed[2] = BIT(sw_option, 6);
	m_pressed[7] = BIT(sw_option, 7);
	m_pressed[5] = BIT(sw_option, 10);
	m_pressed[0] = BIT(sw_option, 11);
	return 0;
}
