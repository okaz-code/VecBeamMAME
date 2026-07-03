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
	flush_stroke();   // emit any stroke still buffered at the frame boundary
	m_display_start = m_display_end;
	m_display_end = m_point_index;
}


uint32_t vectrex_base_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	screen_configuration();

	// Spot killer (deflection protection): the real Vectrex cuts the beam if the deflection stops
	// swinging, so a stationary lit beam cannot burn the phosphor. Model: the beam's total TRAVEL
	// within each m_spotkill_secs window is compared against m_spotkill_dist - below it (deflection
	// dead: crashed / runaway program) the frame is blanked until a window shows movement again.
	const attotime sk_now = machine().time();
	if ((sk_now - m_move_window_start).as_double() >= m_spotkill_secs)
	{
		m_spotkill_engaged = (m_move_accum < m_spotkill_dist);
		m_move_accum = 0.0;
		m_move_window_start = sk_now;
	}
	if (m_spotkill && m_spotkill_engaged)
	{
		m_vector->screen_update(screen, bitmap, cliprect);   // empty list -> black screen (beam cut)
		m_vector->clear_list();
		return 0;
	}

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

		// Straight-line feed of this frame's ring-buffer slice. (The source m_points[] is a ring;
		// m_display_start..m_display_end is the live list.) midChange is still recorded per point and
		// forwarded to the dump (viewer), it just no longer drives any spline subdivision here.
		for (int i = m_display_start; i != m_display_end; i = (i + 1) % NVECT)
		{
			const vectrex_point &p = m_points[i];
			m_vector->set_dump_scale(p.scale);
			m_vector->set_dump_ramp_us(p.ramp_us);
			m_vector->set_dump_midchange(p.midchange);
			m_vector->add_point(p.x, p.y, p.col, p.intensity, p.beam_energy, p.t0, p.t1, p.cap_flags);
		}
	}

	m_vector->screen_update(screen, bitmap, cliprect);
	m_vector->clear_list();
	return 0;
}


/*********************************************************************

   Vector functions

*********************************************************************/

// Per-object-type brightness/width lift. beam_energy is multiplied by a SMOOTH curve of the beam
// intensity (Z) - no hard threshold: below the knee the factor is ~1 (normal: enemies/ship); past the
// adjustable knee it rises (sharpness m_obj_sharp) toward m_obj_max (very prominent: bullets/explosions).
// Parked dots (length-0) additionally get m_obj_star (stars stand out a touch). The renderer's two-regime
// transfer turns the higher energy into extra WIDTH, so prominent objects become several times thicker.
float vectrex_base_state::object_boost(int intensity, bool is_point) const
{
	const float zn = std::clamp(float(intensity) / 255.0f, 0.0f, 1.0f);
	const float t  = std::clamp((zn - m_obj_knee) / std::max(1e-3f, 1.0f - m_obj_knee), 0.0f, 1.0f);
	const float lift = (m_obj_sharp == 1.0f) ? t : powf(t, m_obj_sharp);
	float b = 1.0f + (m_obj_max - 1.0f) * lift;
	if (is_point)
		b *= m_obj_star;
	return b;
}


// Brightness knob (front-panel "INTENSITY"): scale the displayed beam intensity. 50 = x1 (normal), 100 = x2.
// Above ~75% the blanked beam (intensity 0) is lifted to m_bright_floor so the retrace faintly glows, as on a
// real CRT with the brightness cranked. Returns the adjusted intensity (0..255).
int vectrex_base_state::apply_brightness(int intensity) const
{
	if (m_bright_mult == 1.0f && m_bright_floor <= 0)
		return intensity;   // neutral (knob at 50, no retrace floor)
	const int i = (intensity > 0) ? int(intensity * m_bright_mult + 0.5f) : m_bright_floor;
	return std::clamp(i, 0, 255);
}


void vectrex_base_state::add_point(int x, int y, rgb_t color, int intensity, float beam_energy, attotime t0, attotime t1, u32 cap_flags)
{
	vectrex_point *newpoint;

	const int prev_x = m_points[m_point_index].x;   // previous point (before advancing) - for length-0 test
	const int prev_y = m_points[m_point_index].y;

	m_point_index = (m_point_index+1) % NVECT;
	newpoint = &m_points[m_point_index];

	newpoint->x = x;
	newpoint->y = y;
	newpoint->col = color;
	newpoint->intensity = intensity;
	// Object-type lift (length-0 here == a parked dot, e.g. a star/bullet). -1 (untimed) passes through.
	float be = (beam_energy >= 0.0f)
			? beam_energy * object_boost(intensity, x == prev_x && y == prev_y)
			: beam_energy;
	newpoint->beam_energy = be;
	newpoint->cap_flags = u8(cap_flags);   // RAMP on/off terminus bits for the renderer's line caps
	newpoint->t0 = t0;
	newpoint->t1 = t1;
	newpoint->scale = m_cur_scale;     // BIOS vector scale (VIA T1 latch) sampled in update_vector
	newpoint->ramp_us = m_cur_ramp_us; // RAMP-active duration up to this point (for the event dump)
	newpoint->midchange = m_cur_midchange; // curve mid-point (beam velocity changed mid-ramp)
	newpoint->eye = m_imager_status;   // tag with the eye currently being drawn (0 = imager off)
}


void vectrex_base_state::add_point_stereo(int x, int y, rgb_t color, int intensity, float beam_energy, attotime t0, attotime t1, u32 cap_flags)
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
			(int)((m_x_max - x) * SQRT1_2), color, intensity, beam_energy, t0, t1, cap_flags);
}


// Parked-beam (dot) dwell energy. Real-hardware observation (intensity_test / persist_test carts,
// 2026-07-02): a dwelling dot's dazzle onset sits on an I x dt contour - halve the intensity and
// roughly double the dwell time and it dazzles the same - over the whole practical dwell range
// (~30..200us+). So the response must stay near-LINEAR in dt there: x = dt / T_ref with T_ref well
// above the range (default 250us) keeps s = x^g/(x^g+1) in its linear region (g = 1), and the
// saturation (per-area phosphor ceiling) only bends far beyond. This is why "scale" dazzles a dot
// on real hardware: a zero-delta RAMP draw parks the beam for the whole T1 (= scale) period, so
// dt tracks the scale directly. Dots get their OWN ceiling (m_dot_max): a parked beam concentrates
// all its energy in one spot and needs several times the moving-line headroom to span the
// dim -> dazzling -> white-hot continuum the renderer's overdrive expects.
float vectrex_base_state::dot_dwell_energy(int intensity, attotime t0, attotime t1) const
{
	if (t0.is_never() || t1.is_never() || t1 <= t0 || intensity <= 0)
		return -1.0f;
	const double I = std::clamp(double(intensity) / 255.0, 0.0, 1.0);
	const double dt = (t1 - t0).as_double();
	const double x  = dt / m_dot_ref;                           // dwell normalized to the reference
	const double xg = std::pow(std::max(0.0, x), m_dot_curve);
	const double s  = xg / (xg + 1.0);                          // near-linear below T_ref, saturating far above
	const double drive = I * ((1.0 - m_beam_infl) + m_beam_infl * s * m_dot_max);
	return float(std::clamp(drive, 0.0, m_dot_max));
}


// Stroke-aggregate energy. The brightness DENSITY of a RAMP-ON stroke is set by the beam
// SPEED of the whole stroke (one value), not by each tiny sub-segment's own draw time. A slow beam
// deposits more energy per unit length (brighter); BLANK/SR only gate WHICH parts are visible. Because
// the speed is the stroke average, every visible sub-segment of a constant-velocity sweep (e.g. a BIOS
// raster text row) gets the SAME density, instead of each SR-gated dot getting a noisy per-segment dt.
// stroke_speed is in screen pixels / second (0 = no movement -> full dwell density).
float vectrex_base_state::stroke_density_energy(int intensity, double stroke_speed) const
{
	if (intensity <= 0)
		return -1.0f;   // blank move: nothing drawn (renderer falls back, but intensity 0 draws no line)
	const double I = std::clamp(double(intensity) / 255.0, 0.0, 1.0);
	if (stroke_speed <= 1e-6)
		return float(std::clamp(I * m_beam_max, 0.0, m_beam_max));   // parked: maximum dwell density
	// density grows with time-per-unit-length (1/speed); same saturating shape and ceiling as the legacy
	// model so the renderer's two-regime transfer / beam_max still apply. BEAMSPEED is the normalizer.
	const double x  = (1.0 / stroke_speed) * m_beam_speed;
	const double xg = std::pow(std::max(0.0, x), m_beam_curve);
	const double s  = xg / (xg + 1.0);
	const double drive = I * ((1.0 - m_beam_infl) + m_beam_infl * s * m_beam_max);
	return float(std::clamp(drive, 0.0, m_beam_max));
}


// Emit the buffered RAMP-ON stroke. Computes ONE speed from the whole stroke's moved
// distance and elapsed RAMP time, then plays every collected sub-segment out through the normal
// add_point path with that shared density. Called at RAMP-off, ZERO, refresh, or buffer overflow.
void vectrex_base_state::flush_stroke()
{
	if (m_stroke.empty())
		return;

	// total path length (visible + blank moves) in screen pixels; integrator units are 16.16 fixed point
	double path_len = 0.0;
	for (const auto &s : m_stroke)
	{
		const double dx = double(s.x1 - s.x0) / 65536.0;
		const double dy = double(s.y1 - s.y0) / 65536.0;
		path_len += std::sqrt(dx * dx + dy * dy);
	}
	const double ramp_time = (m_stroke.back().t1 - m_stroke.front().t0).as_double();
	const double speed = (path_len > 1e-6 && ramp_time > 1e-12) ? (path_len / ramp_time) : 0.0;

	// A (near-)zero-delta RAMP draw is a PARKED beam dumping its energy into one spot for the whole
	// T1 (= scale) period - this is how games and carts make a dot dazzle with SCALE on real
	// hardware. The speed model degenerates here (speed -> 0 gave a constant "max density"
	// regardless of dwell), so route short-path strokes through the dot dwell-time model instead
	// (per sub-segment lit time, so BLANK gating still masks correctly).
	const bool dot_stroke = (path_len < 0.75);

	// Line end-caps ONLY at the actual RAMP transition with the beam lit AT that instant: the stroke's
	// FIRST sub-segment is the RAMP-on moment (cap its start only if it is lit), the LAST sub-segment is
	// the RAMP-off moment (cap its end only if it is lit). A stroke that opens/closes blanked - e.g. BIOS
	// text, which keeps RAMP on for the whole row and toggles intensity (SR/BLANK) to place dots - gets no
	// cap, since intensity is not "set at RAMP on/off". Game shapes (lit straight from RAMP-on) still cap.
	const int last_idx = int(m_stroke.size()) - 1;
	const bool cap_start = m_stroke[0].intensity > 0;
	const bool cap_end   = m_stroke[last_idx].intensity > 0;

	for (int k = 0; k < int(m_stroke.size()); k++)
	{
		const auto &s = m_stroke[k];
		// Restore the per-segment dump metadata the buffered point carried, then emit.
		m_cur_scale     = s.scale;
		m_cur_ramp_us   = s.ramp_us;
		m_cur_midchange = s.midchange;
		const float e = dot_stroke ? dot_dwell_energy(s.intensity, s.t0, s.t1)
								   : stroke_density_energy(s.intensity, speed);
		const u32 cap = (k == 0 && cap_start ? 1u : 0u) | (k == last_idx && cap_end ? 2u : 0u);   // bit0 start, bit1 end
		(this->*vector_add_point_function)(s.x1, s.y1, s.col, s.intensity, e, s.t0, s.t1, cap);
	}
	m_stroke.clear();
}


// Limit the additive brightness pile-up of repeated dots at the SAME parked location (the renderer
// composites overlapping deposits with BLENDMODE_ADD). The first dot at a new spot is full; further
// dots at the same (x,y) add only until the cumulative beam_energy reaches m_dwell_cap. Moving to a new
// spot resets the accumulator. m_dwell_cap is the "Dwell accum limit" adjuster (0 = first dot only).
float vectrex_base_state::apply_dwell_limit(int x, int y, float energy)
{
	if (energy < 0.0f)
		return energy;   // untimed / invalid: pass through (the renderer falls back to intensity)
	if (x == m_dwell_x && y == m_dwell_y)
	{
		const double remaining = std::max(0.0, m_dwell_cap - m_dwell_accum);
		const double e = std::min(double(energy), remaining);
		m_dwell_accum += e;
		return float(e);
	}
	m_dwell_x = x;
	m_dwell_y = y;
	m_dwell_accum = energy;
	return energy;
}


TIMER_CALLBACK_MEMBER(vectrex_base_state::zero_integrators)
{
	flush_stroke();   // ZERO ends any in-progress RAMP-ON stroke
	// ZERO recentring is real deflection - count it toward the spot-killer travel accumulator
	const int zx = m_x_int, zy = m_y_int;
	m_x_int = m_x_center + (m_analog[A_ZR] * INT_PER_CLOCK);
	m_y_int = m_y_center + (m_analog[A_ZR] * INT_PER_CLOCK);
	{
		const double zdx = double(m_x_int - zx), zdy = double(m_y_int - zy);
		m_move_accum += std::sqrt(zdx * zdx + zdy * zdy);
	}
	m_mid_in_run = false;   // zeroing breaks any connected lit-stroke run
	m_cur_midchange = false;
	(this->*vector_add_point_function)(m_x_int, m_y_int, m_beam_color, 0, -1.0f, attotime::never, attotime::never, 0);
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

		const int vx = m_analog[A_X] + m_analog[A_ZR];   // beam velocity (integrator step / unit length)
		const int vy = m_analog[A_Y] + m_analog[A_ZR];
		m_x_int += length * vx;
		m_y_int += length * vy;

		const int intensity = apply_brightness(2 * m_analog[A_Z] * m_blank);

		// midChange: within a run of connected LIT ramp segments, a vertex is a curve "mid" point when
		// the beam velocity changed there (vs the previous lit segment). The first segment of a run and
		// any blanked move are not mid points (and break the run).
		if (intensity > 0)
		{
			m_cur_midchange = m_mid_in_run && (vx != m_mid_prev_dx || vy != m_mid_prev_dy);
			m_mid_prev_dx = vx;
			m_mid_prev_dy = vy;
			m_mid_in_run = true;
		}
		else
		{
			m_cur_midchange = false;
			m_mid_in_run = false;
		}

		m_cur_ramp_us = (t1 - m_ramp_start_time).as_double() * 1e6;   // RAMP-active time up to this point
		// Defer emission - collect the sub-segment; flush_stroke() at RAMP-off sets the shared stroke
		// density from the whole stroke's speed. Energy is filled in there.
		m_stroke.push_back({ x0, y0, m_x_int, m_y_int, t0, t1, intensity, m_beam_color, m_cur_midchange, m_cur_scale, m_cur_ramp_us });
		if (m_stroke.size() > 8192)   // runaway guard: never let a stuck RAMP grow the buffer unbounded
			flush_stroke();
	}
	else
	{
		m_mid_in_run = false;   // RAMP held: any lit-stroke run ends here
		m_cur_midchange = false;
		if (m_blank)
		{
			const int intensity = apply_brightness(2 * m_analog[A_Z]);
			// RAMP inactive + beam on = a parked dot (e.g. the BIOS Dot routines): energy follows the
			// lit dwell time through the calibrated dot model, independent of the T1 scale (matching
			// real hardware, where fixed-dwell BIOS dots do not respond to scale).
			const float e = apply_dwell_limit(m_x_int, m_y_int,
					dot_dwell_energy(intensity, t0, t1));
			m_cur_ramp_us = 0.0;   // drawn while RAMP inactive (parked dot), not part of a ramp
			(this->*vector_add_point_function)(m_x_int, m_y_int, m_beam_color, intensity, e, t0, t1, 0);
		}
	}

	// Spot killer: the beam is "active" only while it swings beyond the kill radius from screen centre
	// (travel model): accumulate this update's beam movement; screen_update compares the per-window
	// total against m_spotkill_dist - deflection (almost) stopped means the beam is cut. A parked or
	// in-place-jittering beam accumulates ~0, normal drawing accumulates many screen-widths per window.
	{
		const double mdx = double(m_x_int - x0), mdy = double(m_y_int - y0);
		m_move_accum += std::sqrt(mdx * mdx + mdy * mdy);
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
