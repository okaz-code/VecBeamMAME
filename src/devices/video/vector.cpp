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

// Off-screen beam shaping for render_vector_stats::offscreen_energy (monitor glow): a segment
// contributes when its beam energy exceeds the floor AND an endpoint is at least this far outside
// the visible area (in screen fractions). These match the defaults of the renderer sliders they
// replace (mglow_threshold / mglow_min_distance); baking them keeps the device -> renderer
// interface a plain per-frame aggregate.
static constexpr float OFFSCREEN_ENERGY_MIN = 0.7f;
static constexpr float OFFSCREEN_MARGIN     = 0.30f;

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

	// Legacy random flicker (-flicker): applied to every drawn point in full-frame mode.
	if (vector_options::s_flicker && (intensity > 0))
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
	newpoint->x0 = (m_vector_index > 0) ? m_vector_list[m_vector_index - 1].x : x;
	newpoint->y0 = (m_vector_index > 0) ? m_vector_list[m_vector_index - 1].y : y;
	newpoint->col = color;
	newpoint->intensity = intensity;
	// Beam energy carried on the primitive for renderer overdrive effects, in the unified convention:
	//   0..1 = normal display range, 1..N = overdrive (slow sweeps / dwelling dots concentrate energy),
	//   < 0  = "no information" (the source did not measure beam energy).
	// When the device supplies a value (beam_energy >= 0) it is passed through unchanged (clamped to a
	// sane upper bound only); a NEGATIVE value is preserved AS-IS so the renderer can tell "no info"
	// apart from "energy 0" and derive its own energy from the per-segment timestamps (unified model).
	// This is pure data: the displayed intensity above is untouched, so renderers that ignore beam_energy
	// (and the renderer's own fallback, n = clamp(color.a)) produce identical stock output.
	newpoint->beam_energy = (beam_energy >= 0.0f) ? std::clamp(beam_energy, 0.0f, 16.0f)
												  : beam_energy;
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
	m_vector_index = 0;
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

	// Per-frame statistics for the render container (see render_vector_stats): total beam energy
	// (EHT load) and shaped off-screen energy (monitor glow), accumulated below once per beam
	// event (window-boundary re-emissions do not recount, matching the notifiers).
	float stats_total_energy = 0.0f;
	float stats_offscreen_energy = 0.0f;

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

				if (curpoint->beam_energy > 0.0f)
				{
					// total beam energy = current (beam_energy) x draw time (proportional to
					// normalized length at constant velocity), summed over the frame
					const float lx = coords.x1 - coords.x0, ly = coords.y1 - coords.y0;
					stats_total_energy += curpoint->beam_energy * sqrtf(lx * lx + ly * ly);
					// shaped off-screen contribution (see OFFSCREEN_ENERGY_MIN / OFFSCREEN_MARGIN)
					auto outside = [] (float x, float y) {
						const float dx = (x < 0.0f) ? -x : (x > 1.0f) ? (x - 1.0f) : 0.0f;
						const float dy = (y < 0.0f) ? -y : (y > 1.0f) ? (y - 1.0f) : 0.0f;
						return std::max(dx, dy);
					};
					if (curpoint->beam_energy > OFFSCREEN_ENERGY_MIN
						&& std::max(outside(coords.x0, coords.y0), outside(coords.x1, coords.y1)) > OFFSCREEN_MARGIN)
						stats_offscreen_energy += curpoint->beam_energy - OFFSCREEN_ENERGY_MIN;
				}
			}
		}
		else if (!curpoint->emitted)
		{
			m_move_notifier(curpoint->x, curpoint->y, curpoint->col, visarea.width(), visarea.height());
		}

		curpoint->emitted = true;

		curpoint++;
	}

	m_frame_end_notifier();

	// Publish this frame's statistics into the screen container; render_target propagates them
	// onto the primitive list, where a renderer can read them without touching this device.
	render_vector_stats stats;
	stats.frame_id = ++m_stats_frame_id;
	stats.list_generation = m_list_generation;
	stats.list_stale = m_beam_list_stale;
	stats.timed = m_avg_timing;
	stats.total_energy = stats_total_energy;
	stats.offscreen_energy = stats_offscreen_energy;
	screen.container().set_vector_stats(stats);

	return 0;
}
