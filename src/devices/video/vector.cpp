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
// contributes when its beam energy exceeds this floor (matches the default of the old
// mglow_threshold renderer slider it replaces). The minimum off-screen distance is NOT baked
// here: the energy is published binned by excursion depth and the renderer applies its own
// cutoff (the Monitor Glow Min Distance slider, mglow_min_distance).
static constexpr float OFFSCREEN_ENERGY_MIN = 0.7f;

// Localized bezel-edge glow (render_vector_stats::edge_energy): the phosphor face continues behind
// the bezel, so a beam driven off-screen lights the border near its exit point. A deeper excursion
// deposits more energy behind the bezel: use a rising exponential response (normalized screen
// fractions to 63% at EDGE_GLOW_REACH), capped once the emulated unbounded integrator is beyond the
// physical deflection region. No energy floor here (unlike the monitor-glow shaping above): the real
// edge light comes from MANY medium-energy passes accumulating per frame, not from rare intense
// events, so every lit off-screen contribution counts and the renderer's gain slider scales the sum.
static constexpr float EDGE_GLOW_REACH     = 0.12f;
static constexpr float EDGE_GLOW_DEPTH_CAP = 0.30f;

// Bin one lit segment's off-screen portion onto the border it left through. coords are normalized
// screen coords (may lie outside [0,1]). The event's energy is scaled by the fraction of the
// segment that is outside (a fully-outside segment or parked dot counts in full) and deposited
// at the outside part's midpoint: nearest border edge, distance-amplified with saturation, into that edge's bin.
static void accumulate_edge_glow(render_bounds const &coords, float beam_energy, float (&edge)[4][render_vector_stats::EDGE_GLOW_BINS])
{
	const float e_over = beam_energy;
	if (e_over <= 0.0f)
		return;
	const float x0 = coords.x0, y0 = coords.y0, x1 = coords.x1, y1 = coords.y1;
	if (x0 >= 0.0f && x0 <= 1.0f && y0 >= 0.0f && y0 <= 1.0f
		&& x1 >= 0.0f && x1 <= 1.0f && y1 >= 0.0f && y1 <= 1.0f)
		return;   // fully inside

	const float dx = x1 - x0, dy = y1 - y0;

	// Liang-Barsky: the parameter interval [tmin, tmax] of the segment inside the unit square
	float tmin = 0.0f, tmax = 1.0f;
	auto clip1 = [&](float p, float q) {
		if (fabsf(p) < 1e-9f) { if (q < 0.0f) { tmin = 1.0f; tmax = 0.0f; } return; }
		const float r = q / p;
		if (p < 0.0f) tmin = std::max(tmin, r); else tmax = std::min(tmax, r);
	};
	clip1(-dx, x0);          // x >= 0
	clip1( dx, 1.0f - x0);   // x <= 1
	clip1(-dy, y0);          // y >= 0
	clip1( dy, 1.0f - y0);   // y <= 1

	auto deposit = [&](float mx, float my, float e) {
		const float cx = std::clamp(mx, 0.0f, 1.0f), cy = std::clamp(my, 0.0f, 1.0f);
		const float ox = fabsf(mx - cx), oy = fabsf(my - cy);
		int side; float along;
		if (ox >= oy) { side = (mx < cx) ? 0 : 1; along = cy; }
		else          { side = (my < cy) ? 2 : 3; along = cx; }
		const int bin = std::clamp(int(along * render_vector_stats::EDGE_GLOW_BINS), 0, render_vector_stats::EDGE_GLOW_BINS - 1);
		edge[side][bin] += e * (1.0f - expf(-std::min(std::max(ox, oy), EDGE_GLOW_DEPTH_CAP) / EDGE_GLOW_REACH));
	};

	if (tmin >= tmax)
	{
		// no part inside (includes parked dots outside): everything at the midpoint
		deposit((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, e_over);
		return;
	}

	// outside tails [0, tmin) and (tmax, 1]: event energy scaled by the outside fraction and
	// split between the tails in proportion to their lengths
	const float w0 = std::max(tmin, 0.0f), w1 = std::max(1.0f - tmax, 0.0f);
	const float out_frac = std::clamp(w0 + w1, 0.0f, 1.0f);
	if (out_frac <= 1e-6f)
		return;
	const float E = e_over * out_frac;
	const float wsum = w0 + w1;
	if (w0 > 0.0f) deposit(x0 + dx * (tmin * 0.5f),          y0 + dy * (tmin * 0.5f),          E * (w0 / wsum));
	if (w1 > 0.0f) deposit(x0 + dx * ((1.0f + tmax) * 0.5f), y0 + dy * ((1.0f + tmax) * 0.5f), E * (w1 / wsum));
}

float vector_options::s_flicker = 0.0f;
float vector_options::s_beam_width_min = 0.0f;
float vector_options::s_beam_width_max = 0.0f;
float vector_options::s_beam_dot_size = 0.0f;
float vector_options::s_beam_intensity_weight = 0.0f;
float vector_options::s_overscan_x = 1.0f;
float vector_options::s_overscan_y = 1.0f;
float vector_options::s_blank_leak = 0.0f;

void vector_options::init(emu_options &options)
{
	s_beam_width_min = options.beam_width_min();
	s_beam_width_max = options.beam_width_max();
	s_beam_dot_size = options.beam_dot_size();
	s_beam_intensity_weight = options.beam_intensity_weight();
	s_flicker = options.flicker();
	s_overscan_x = options.vector_overscan_x();
	s_overscan_y = options.vector_overscan_y();
	s_blank_leak = options.vector_blank_leak();
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
			m_event_dump << "frame,t0,t1,draw_us,ramp_us,scale,x0,y0,x,y,length,intensity,beam_energy,midchange,col\n";   // frame=list generation; draw_us=segment draw time (us); ramp_us=RAMP-active time up to this point (us); scale=BIOS vector scale (VIA T1 latch); midchange=curve mid-point (beam velocity changed mid-ramp); col=rgb hex
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

	// Legacy random flicker (-flicker): the same random reduction is applied to the display
	// intensity AND to a device-supplied beam energy. Renderers that derive brightness from
	// beam_energy (the HDR vector chains; only Star Wars supplies it device-side) would otherwise
	// never see the intensity reduction and show no flicker at all, while every generic-energy
	// game (Tempest etc., beam_energy < 0, derived from the post-flicker intensity) did.
	if (vector_options::s_flicker && (intensity > 0))
	{
		float random = float(machine().rand() & 255) / 255.0f; // random value between 0.0 and 1.0
		const float reduction = random * vector_options::s_flicker;

		intensity -= int(intensity * reduction);

		intensity = std::clamp(intensity, 0, 255);

		if (beam_energy > 0.0f)
			beam_energy -= beam_energy * reduction;
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
		util::stream_format(m_event_dump, "%u,%.9f,%.9f,%.3f,%.3f,%d,%d,%d,%d,%d,%.3f,%d,%.4f,%d,%06x\n",
				m_list_generation, t0.as_double(), t1.as_double(), draw_us, m_dump_ramp_us, m_dump_scale,
				newpoint->x0, newpoint->y0, x, y, seg_len, dump_intensity, newpoint->beam_energy, m_dump_midchange ? 1 : 0,
				u32(newpoint->col) & 0xffffff);
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
	float stats_offscreen_energy[render_vector_stats::OFFSCREEN_DEPTH_BINS] = {};
	float stats_edge_energy[4][render_vector_stats::EDGE_GLOW_BINS] = {};

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
					// shaped off-screen contribution (see OFFSCREEN_ENERGY_MIN), binned by how far
					// beyond the visible area the segment reaches so the renderer can apply its
					// own minimum-distance cutoff (mglow_min_distance slider)
					auto outside = [] (float x, float y) {
						const float dx = (x < 0.0f) ? -x : (x > 1.0f) ? (x - 1.0f) : 0.0f;
						const float dy = (y < 0.0f) ? -y : (y > 1.0f) ? (y - 1.0f) : 0.0f;
						return std::max(dx, dy);
					};
					if (curpoint->beam_energy > OFFSCREEN_ENERGY_MIN)
					{
						const float depth = std::max(outside(coords.x0, coords.y0), outside(coords.x1, coords.y1));
						if (depth > 0.0f)
						{
							const int bin = std::min(render_vector_stats::OFFSCREEN_DEPTH_BINS - 1,
									int(depth / render_vector_stats::OFFSCREEN_DEPTH_STEP));
							stats_offscreen_energy[bin] += curpoint->beam_energy - OFFSCREEN_ENERGY_MIN;
						}
					}
					// localized bezel-edge glow bins (own inside/outside test - no margin, distance-decayed)
					accumulate_edge_glow(coords, curpoint->beam_energy, stats_edge_energy);
				}
			}
		}
		else
		{
			// Blanked beam move (intensity 0). Normally invisible, but a real CRT leaks a little light
			// during the blanked retrace/move; s_blank_leak > 0 draws the move path faintly so it shows.
			// beam_energy = -1 so the renderer derives the level from the move's speed (a fast jump
			// leaks less than a slow move); no end caps. Emitted every pass (like lit lines) so the
			// window-boundary re-blend works; the move notifier still fires once (below).
			if (vector_options::s_blank_leak > 0.0f)
			{
				const int leak_i = std::clamp(int(vector_options::s_blank_leak * 255.0f + 0.5f), 1, 255);
				screen.container().add_line(
						coords.x0, coords.y0, coords.x1, coords.y1,
						beam_width,
						(leak_i << 24) | (curpoint->col & 0xffffff),
						flags,
						-1.0f,
						curpoint->t0.is_never() ? -1.0 : curpoint->t0.as_double(),
						curpoint->t1.is_never() ? -1.0 : curpoint->t1.as_double(),
						0);
			}
			if (!curpoint->emitted)
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
	static_assert(sizeof(stats.offscreen_energy) == sizeof(stats_offscreen_energy));
	memcpy(stats.offscreen_energy, stats_offscreen_energy, sizeof(stats.offscreen_energy));
	static_assert(sizeof(stats.edge_energy) == sizeof(stats_edge_energy));
	memcpy(stats.edge_energy, stats_edge_energy, sizeof(stats.edge_energy));
	screen.container().set_vector_stats(stats);

	return 0;
}
