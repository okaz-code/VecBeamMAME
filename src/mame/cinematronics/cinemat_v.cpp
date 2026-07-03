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
	int intensity = 0xff;

	// Beam-event timing: stamp each vector with the machine time it is drawn (this callback runs during
	// CCPU execution). The CCPU redraw is VBLANK-locked, so the timestamp is frame-grained; that is
	// enough for Cinematronics' characteristic OVERLOAD flicker - when the image needs more than one
	// frame to draw, consecutive frames' vectors get distinct times, letting the bgfx beam-event
	// renderer retain and composite the partial frames instead of dropping them. No-op when beam events
	// are off (renderer enables them via -bgfx_vec_beam_events; the window weight ignores t0 then).
	const attotime now = machine().time();

	/* adjust for slop */
	sx -= visarea.left();
	ex -= visarea.left();
	sy -= visarea.top();
	ey -= visarea.top();

	/* point intensity / dwell is determined by the shift value */
	//
	// A degenerate vector (sx==ex && sy==ey) is a dwelling dot: the beam parks at the spot and its
	// brightness comes from how long it dwells there, which the CCPU encodes in the DV timer register T
	// (passed here as `shift`; see ccpu.cpp:482 DV, delta >> m_T). The stock code turned shift straight
	// into a display intensity `0x1ff * shift / 8` (511*shift/8) - which overflows the 8-bit intensity
	// field for shift > 3 (e.g. shift 4 -> 255, shift 5 -> 319 -> wraps to 63) and, being frame-grained
	// (t0==t1), carries NO dwell information for the renderer's unified energy model.
	//
	// Instead we (a) clamp the DISPLAY intensity to a valid 0..255 (removing the wrap bug) and (b) encode
	// the dwell as a real time span t1-t0 so the renderer's per-dot energy model (drawbgfx generic_beam_energy,
	// dot branch) derives the overdrive from it, in the unified convention.
	//
	// shift -> dwell time: the exact CCPU->CRT sweep time is not tracked by MAME (the DV op issues in one
	// CPU cycle while the analog vector generator sweeps for a length/timer-dependent interval; see
	// vector-engine-beam-timing-survey.md sec.5, which notes t0~=t1 for this hardware). We use the same
	// LINEAR-in-shift relationship the stock brightness formula implied (brightness ~ dwell for a CRT dot),
	// with an approximate base of DWELL_US_PER_SHIFT us per shift step chosen so the brightest dots
	// (shift ~= 4) dwell ~120us, i.e. a few x the renderer's default dot reference (energy_dot_ref 30us) and
	// thus read as genuine overdrive. This is an APPROXIMATION (base value, not a measured CCPU->sweep
	// conversion); adjust DWELL_US_PER_SHIFT / the chain's energy_dot_ref to taste.
	static constexpr double DWELL_US_PER_SHIFT = 30.0;
	attotime t1 = now;
	if (sx == ex && sy == ey)
	{
		intensity = std::min(0x1ff * shift / 8, 0xff);            // display intensity, clamped (was: 8-bit wrap bug)
		t1 = now + attotime::from_usec(int(shift * DWELL_US_PER_SHIFT + 0.5)); // dwell span -> renderer dot energy
	}

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
