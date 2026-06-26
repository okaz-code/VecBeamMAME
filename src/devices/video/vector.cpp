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
float vector_options::s_beam_width_min = 0.0f;
float vector_options::s_beam_width_max = 0.0f;
float vector_options::s_beam_dot_size = 0.0f;
float vector_options::s_beam_intensity_weight = 0.0f;
float vector_options::s_overscan_x = 1.0f;
float vector_options::s_overscan_y = 1.0f;

void vector_options::init(emu_options &options)
{
	s_beam_width_min = options.beam_width_min();
	s_beam_width_max = options.beam_width_max();
	s_beam_dot_size = options.beam_dot_size();
	s_beam_intensity_weight = options.beam_intensity_weight();
	s_flicker = options.flicker();
	s_overscan_x = options.vector_overscan_x();
	s_overscan_y = options.vector_overscan_y();
}

// device type definition
DEFINE_DEVICE_TYPE(VECTOR, vector_device, "vector_device", "VECTOR")

vector_device::vector_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, VECTOR, tag, owner, clock),
		device_video_interface(mconfig, *this),
		m_vector_list(nullptr),
		m_min_intensity(255),
		m_max_intensity(0),
		m_beam_event_mode(false),
		m_list_generation(0),
		m_last_drawn_generation(~uint32_t(0)),
		m_beam_list_stale(false)
{
}

void vector_device::device_start()
{
	vector_options::init(machine().options());

	m_vector_index = 0;

	/* allocate memory for tables */
	m_vector_list = std::make_unique<point[]>(MAX_POINTS);

	// Debug aid: -vector_event_dump <file> writes one CSV row per timed beam event.
	const char *const dump_path = machine().options().vector_event_dump();
	if (dump_path != nullptr && dump_path[0] != '\0')
	{
		m_event_dump.open(dump_path);
		if (m_event_dump.is_open())
			m_event_dump << "frame,t0,t1,draw_us,ramp_us,scale,x0,y0,x,y,length,intensity,beam_energy,midchange\n";   // frame=list generation; draw_us=segment draw time (us); ramp_us=RAMP-active time up to this point (us); scale=BIOS vector scale (VIA T1 latch); midchange=curve mid-point (beam velocity changed mid-ramp)
		else
			osd_printf_warning("vector: could not open event dump file '%s'\n", dump_path);
	}
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

util::notifier_subscription vector_device::add_beam_energy_line_notifier(beam_energy_line_delegate &&n)
{
	return m_beam_energy_line_notifier.subscribe(std::move(n));
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

void vector_device::add_point(int x, int y, rgb_t color, int intensity, float beam_energy, attotime t0, attotime t1, u32 cap_flags)
{
	point *newpoint;

	intensity = std::clamp(intensity, 0, 255);

	m_min_intensity = intensity > 0 ? std::min(m_min_intensity, intensity) : m_min_intensity;
	m_max_intensity = intensity > 0 ? std::max(m_max_intensity, intensity) : m_max_intensity;

	// True (pre-flicker) intensity for the event dump: the random -flicker reduction below is a display
	// effect, so the analysis log should record the value the source actually produced (stable, not jittered).
	const int dump_intensity = intensity;

	// Legacy random flicker (-flicker). Skipped for timed points when a beam-event renderer is
	// attached: those flicker physically through the time-window assignment, and the random
	// jitter would only distort it. Untimed sources and classic-mode rendering keep the option.
	const bool physical_flicker = m_beam_event_mode && !t0.is_never();
	if (vector_options::s_flicker && (intensity > 0) && !physical_flicker)
	{
		float random = float(machine().rand() & 255) / 255.0f; // random value between 0.0 and 1.0

		intensity -= int(intensity * random * vector_options::s_flicker);

		intensity = std::clamp(intensity, 0, 255);
	}

	newpoint = &m_vector_list[m_vector_index];
	newpoint->x = x;
	newpoint->y = y;
	// Capture the segment start = the previous beam position (the immediately preceding point in draw
	// order) while the chain is intact. A degenerate first point (no predecessor) starts at itself.
	// In beam-event mode the predecessor of the first new-frame point is the retained tail's last entry,
	// but that point is an intensity-0 move (no line drawn), so the chain re-anchors correctly from there.
	newpoint->x0 = (m_vector_index > 0) ? m_vector_list[m_vector_index - 1].x : x;
	newpoint->y0 = (m_vector_index > 0) ? m_vector_list[m_vector_index - 1].y : y;
	newpoint->col = color;
	newpoint->intensity = intensity;
	// Normalized (0..1) beam energy carried on the primitive for renderer overdrive effects.
	// When the device supplies a raw value (beam_energy >= 0) it is used as-is; otherwise
	// the displayed intensity is used as the normalized value. This is pure data: the displayed
	// intensity above is unchanged, so renderers that ignore it produce stock output.
	// Upper bound 8.0 (not 1.0): a raw beam_energy can exceed peak (dwelling beam concentrates energy);
	// the renderer clamps to 0..1 for the displayed core and uses the raw >1 part for the overdrive/HDR
	// white-hot flare. Sources that never exceed 1.0 are unaffected.
	newpoint->beam_energy = (beam_energy >= 0.0f) ? std::clamp(beam_energy, 0.0f, 16.0f)
												  : float(intensity) / 255.0f;
	newpoint->t0 = t0;
	newpoint->t1 = t1;
	newpoint->cap_flags = cap_flags;
	newpoint->emitted = false;

	if (m_event_dump.is_open() && !t0.is_never())
	{
		const double seg_len = std::sqrt(double(x - newpoint->x0) * double(x - newpoint->x0)
				+ double(y - newpoint->y0) * double(y - newpoint->y0));
		const double draw_us = (t1 - t0).as_double() * 1e6;   // actual draw time = realized beam scale
		util::stream_format(m_event_dump, "%u,%.9f,%.9f,%.3f,%.3f,%d,%d,%d,%d,%d,%.3f,%d,%.4f,%d\n",
				m_list_generation, t0.as_double(), t1.as_double(), draw_us, m_dump_ramp_us, m_dump_scale,
				newpoint->x0, newpoint->y0, x, y, seg_len, dump_intensity, newpoint->beam_energy, m_dump_midchange ? 1 : 0);
	}

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
	if (m_beam_event_mode)
	{
		// Timed points are presentation-owned: they stay queued until screen_update has emitted
		// them once, so a list crossing a frame boundary keeps its tail. Only untimed points
		// (stock semantics) are dropped here.
		int w = 0;
		for (int r = 0; r < m_vector_index; r++)
		{
			if (!m_vector_list[r].t0.is_never())
				m_vector_list[w++] = m_vector_list[r];
		}
		m_vector_index = w;
	}
	else
	{
		m_vector_index = 0;
	}
	// A new beam list is starting; bump the generation so screen_update can tell this frame redrew.
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

	curpoint = m_vector_list.get();

	screen.container().empty();
	screen.container().add_rect(0.0f, 0.0f, 1.0f, 1.0f, rgb_t(0xff,0x00,0x00,0x00), PRIMFLAG_BLENDMODE(BLENDMODE_ALPHA) | PRIMFLAG_VECTORBUF(1));

	m_frame_begin_notifier();

	// CRT-flicker detection: stale = the beam list was not refreshed since the last draw. A renderer
	// reads this via beam_list_stale(); the emulated output here is unchanged.
	m_beam_list_stale = (m_list_generation == m_last_drawn_generation);
	m_last_drawn_generation = m_list_generation;

	for (int i = 0; i < m_vector_index; i++)
	{
		render_bounds coords;

		float intensity = (float)curpoint->intensity / 255.0f;
		float intensity_weight = normalized_sigmoid(intensity, vector_options::s_beam_intensity_weight);

		// check for static intensity
		float beam_width = m_min_intensity == m_max_intensity
			? vector_options::s_beam_width_min
			: vector_options::s_beam_width_min + intensity_weight * (vector_options::s_beam_width_max - vector_options::s_beam_width_min);

		// normalize width
		beam_width *= 1.0f / (float)VECTOR_WIDTH_DENOM;

		// apply point scale for points
		if (curpoint->x0 == curpoint->x && curpoint->y0 == curpoint->y)
			beam_width *= vector_options::s_beam_dot_size;

		coords.x0 = (float(curpoint->x0) - xoffs) * xscale;
		coords.y0 = (float(curpoint->y0) - yoffs) * yscale;
		coords.x1 = (float(curpoint->x) - xoffs) * xscale;
		coords.y1 = (float(curpoint->y) - yoffs) * yscale;

		// Overscan zoom about the 0.5 screen centre (1.0 = none). < 1.0 shrinks the image, revealing
		// the off-screen beams kept by the symmetric clip window; the renderer clips at the edge.
		if (vector_options::s_overscan_x != 1.0f || vector_options::s_overscan_y != 1.0f)
		{
			coords.x0 = (coords.x0 - 0.5f) * vector_options::s_overscan_x + 0.5f;
			coords.y0 = (coords.y0 - 0.5f) * vector_options::s_overscan_y + 0.5f;
			coords.x1 = (coords.x1 - 0.5f) * vector_options::s_overscan_x + 0.5f;
			coords.y1 = (coords.y1 - 0.5f) * vector_options::s_overscan_y + 0.5f;
		}

		if (curpoint->intensity != 0)
		{
			screen.container().add_line(
					coords.x0, coords.y0, coords.x1, coords.y1,
					beam_width,
					(curpoint->intensity << 24) | (curpoint->col & 0xffffff),
					flags,
					curpoint->beam_energy,
					curpoint->t0.is_never() ? -1.0 : curpoint->t0.as_double(),
					curpoint->t1.is_never() ? -1.0 : curpoint->t1.as_double(),
					curpoint->cap_flags);
			// Points surviving into a second emission (window-boundary blend) re-emit their
			// primitive but must not re-fire the notifiers: one beam event, one notification.
			if (!curpoint->emitted)
			{
				m_line_notifier(curpoint->x0, curpoint->y0, curpoint->x, curpoint->y, curpoint->col, curpoint->intensity, visarea.width(), visarea.height());
				// Parallel notifier: normalized-space endpoints + beam energy, for off-screen beam effects.
				m_beam_energy_line_notifier(coords.x0, coords.y0, coords.x1, coords.y1, curpoint->beam_energy);
			}
		}
		else if (!curpoint->emitted)
		{
			m_move_notifier(curpoint->x, curpoint->y, curpoint->col, visarea.width(), visarea.height());
		}

		curpoint->emitted = true;

		curpoint++;
	}

	if (m_beam_event_mode)
	{
		// Timed points are consumed once emitted, except those young enough to still carry
		// energy into the next window (the renderer's beam-integration window, slider max 100ms):
		// they survive until they age past `keep` so the integration window can still draw them. Their
		// emitted flag keeps the notifiers above from firing twice for the same beam event.
		// Untimed points keep stock semantics (redrawn until the next clear_list).
		const attotime now = machine().time();
		const attotime keep = attotime::from_msec(110);
		int w = 0;
		for (int i = 0; i < m_vector_index; i++)
		{
			const point &pt = m_vector_list[i];
			if (pt.t0.is_never() || (pt.t0 + keep > now))
				m_vector_list[w++] = pt;
		}
		m_vector_index = w;
	}

	m_frame_end_notifier();

	return 0;
}
