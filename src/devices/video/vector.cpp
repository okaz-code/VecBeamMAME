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
#include "input.h" // for the MVEC playback tool keys
#include "render.h"
#include "screen.h"
#include "ui/uimain.h"

#include "util/hashing.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


#define VECTOR_WIDTH_DENOM 512

// 20000 is needed for mhavoc (see MT 06668) 10000 is enough for other games
#define MAX_POINTS 20000

// Off-screen beam shaping for render_vector_stats::offscreen_energy (monitor glow): a segment
// contributes when its beam energy exceeds this floor (matches the default of the old
// mglow_threshold renderer slider it replaces). The minimum off-screen distance is NOT baked
// here: the energy is published binned by excursion depth and the renderer applies its own
// cutoff (the Monitor Glow Min Distance slider, mglow_min_distance).
static constexpr float OFFSCREEN_ENERGY_MIN = 0.7f;
static constexpr float MONITOR_GLOW_ENERGY_CAP = 1.0f;
static constexpr float MONITOR_GLOW_SAMPLE_STEP = 0.04f;

// Record whether overloaded deflection surrounds the tube, rather than summing every off-screen
// vector into one count-sensitive scalar. Each angle/depth cell stores a capped peak: drawing many
// concentric arcs in one direction cannot outweigh missing parts of the outer-face scan.
static void accumulate_monitor_glow_coverage(render_bounds const &coords, float beam_energy,
		float (&coverage)[render_vector_stats::MONITOR_GLOW_ANGLE_BINS][render_vector_stats::OFFSCREEN_DEPTH_BINS])
{
	if (beam_energy <= OFFSCREEN_ENERGY_MIN)
		return;

	const float dx = coords.x1 - coords.x0, dy = coords.y1 - coords.y0;
	const float length = sqrtf(dx * dx + dy * dy);
	const int steps = std::clamp(int(ceilf(length / MONITOR_GLOW_SAMPLE_STEP)), 1, 128);
	const float energy = std::min(beam_energy - OFFSCREEN_ENERGY_MIN, MONITOR_GLOW_ENERGY_CAP);
	static constexpr float ANGLE_SCALE = float(render_vector_stats::MONITOR_GLOW_ANGLE_BINS) / 6.2831853071795864769f;

	for (int step = 0; step <= steps; ++step)
	{
		const float t = float(step) / float(steps);
		const float x = coords.x0 + dx * t, y = coords.y0 + dy * t;
		const float depth = std::max({ -x, x - 1.0f, -y, y - 1.0f, 0.0f });
		if (depth <= 0.0f)
			continue;

		float angle = atan2f(y - 0.5f, x - 0.5f);
		if (angle < 0.0f)
			angle += 6.2831853071795864769f;
		const int angle_bin = std::clamp(int(angle * ANGLE_SCALE), 0, render_vector_stats::MONITOR_GLOW_ANGLE_BINS - 1);
		const int depth_bin = std::min(render_vector_stats::OFFSCREEN_DEPTH_BINS - 1,
				int(depth / render_vector_stats::OFFSCREEN_DEPTH_STEP));
		coverage[angle_bin][depth_bin] = std::max(coverage[angle_bin][depth_bin], energy);
	}
}

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

namespace {

constexpr u32 MVEC_FRAME_MAGIC = 0x4d415246U; // "FRAM" in little-endian byte order
constexpr u16 MVEC_VERSION_MAJOR = 1;
constexpr u16 MVEC_VERSION_MINOR = 1;
constexpr size_t MVEC_QUEUE_LIMIT = 256U * 1024U * 1024U;
std::mutex s_mvec_claim_mutex;
bool s_mvec_claimed = false;

void append_u8(std::vector<u8> &out, u8 value) { out.push_back(value); }
void append_u16(std::vector<u8> &out, u16 value)
{
	append_u8(out, u8(value)); append_u8(out, u8(value >> 8));
}
void append_u32(std::vector<u8> &out, u32 value)
{
	for (unsigned shift = 0; shift < 32; shift += 8) append_u8(out, u8(value >> shift));
}
void append_u64(std::vector<u8> &out, u64 value)
{
	for (unsigned shift = 0; shift < 64; shift += 8) append_u8(out, u8(value >> shift));
}
void append_i32(std::vector<u8> &out, s32 value) { append_u32(out, u32(value)); }
void append_i64(std::vector<u8> &out, s64 value) { append_u64(out, u64(value)); }
void append_float(std::vector<u8> &out, float value)
{
	u32 bits; std::memcpy(&bits, &value, sizeof(bits)); append_u32(out, bits);
}
void append_double(std::vector<u8> &out, double value)
{
	u64 bits; std::memcpy(&bits, &value, sizeof(bits)); append_u64(out, bits);
}
void append_string(std::vector<u8> &out, std::string_view value)
{
	if (value.size() > std::numeric_limits<u16>::max())
		fatalerror("MVEC metadata string is too long\n");
	append_u16(out, u16(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

class payload_reader
{
public:
	payload_reader(const std::vector<u8> &data) : m_data(data) { }
	u8 get_u8() { require(1); return m_data[m_pos++]; }
	u16 get_u16() { u16 v = get_u8(); v |= u16(get_u8()) << 8; return v; }
	u32 get_u32() { u32 v = 0; for (unsigned s = 0; s < 32; s += 8) v |= u32(get_u8()) << s; return v; }
	u64 get_u64() { u64 v = 0; for (unsigned s = 0; s < 64; s += 8) v |= u64(get_u8()) << s; return v; }
	s32 get_i32() { return s32(get_u32()); }
	s64 get_i64() { return s64(get_u64()); }
	float get_float() { u32 b = get_u32(); float v; std::memcpy(&v, &b, sizeof(v)); return v; }
	double get_double() { u64 b = get_u64(); double v; std::memcpy(&v, &b, sizeof(v)); return v; }
	bool done() const { return m_pos == m_data.size(); }
private:
	void require(size_t count) { if (count > m_data.size() - m_pos) fatalerror("MVEC frame is truncated\n"); }
	const std::vector<u8> &m_data;
	size_t m_pos = 0;
};

} // anonymous namespace

class vector_device::stream_state
{
public:
	enum class mode { NONE, RECORD, PLAYBACK };

	stream_state(vector_device &owner) : m_owner(owner)
	{
		const char *const record_path = owner.machine().options().vector_record();
		const char *const playback_path = owner.machine().options().vector_playback();
		const bool record = record_path && record_path[0];
		const bool playback = playback_path && playback_path[0];
		if (record && playback)
		{
			osd_printf_error("MVEC: -vector_record and -vector_playback are mutually exclusive; both are disabled\n");
			return;
		}
		if (!record && !playback)
			return;

		{
			std::lock_guard<std::mutex> lock(s_mvec_claim_mutex);
			if (s_mvec_claimed)
			{
				osd_printf_warning("MVEC: only one vector device per machine is supported; ignoring device '%s'\n", m_owner.tag());
				return;
			}
			s_mvec_claimed = true;
			m_claimed = true;
		}

		if (record)
		{
			m_mode = mode::RECORD;
			m_recorded_frame_period = m_owner.screen().frame_period().attoseconds();
			m_output.open(record_path, std::ios::binary | std::ios::trunc);
			if (!m_output)
			{
				osd_printf_error("MVEC: unable to create record file '%s'; recording is disabled\n", record_path);
				m_mode = mode::NONE;
				release_claim();
				return;
			}
			try
			{
				write_header();
			}
			catch (emu_fatalerror const &err)
			{
				osd_printf_error("MVEC: unable to initialise record file '%s': %s; recording is disabled\n", record_path, err.what());
				m_output.close();
				m_mode = mode::NONE;
				release_claim();
				return;
			}
			m_writer = std::thread(&stream_state::writer_loop, this);
			osd_printf_info("MVEC: recording vector stream to %s\n", record_path);
		}
		else
		{
			m_mode = mode::PLAYBACK;
			m_input.open(playback_path, std::ios::binary);
			if (!m_input)
			{
				osd_printf_error("MVEC: playback file '%s' was not found or could not be opened; playback is disabled\n", playback_path);
				m_mode = mode::NONE;
				release_claim();
				return;
			}
			try
			{
				read_header();
			}
			catch (emu_fatalerror const &err)
			{
				osd_printf_error("MVEC: unable to load playback file '%s': %s; playback is disabled\n", playback_path, err.what());
				m_input.close();
				m_mode = mode::NONE;
				release_claim();
				return;
			}
			// MVEC contains final vector primitives, not an emulated audio timeline. Keep the game
			// machine running to feed the renderer/UI, but never expose its unrelated frame-zero audio
			// after a seek or while the vector stream is paused.
			m_owner.machine().sound().vector_playback_mute(true);
			// The overlay is on unless asked for otherwise. Alt+O still toggles it either way, and
			// the go-to prompt still forces it visible - a hidden modal input reads as a lock-up.
			m_tool_overlay = m_owner.machine().options().vector_playback_overlay();
			m_tool_announced = true;
			show_tool_message("Loaded");
			osd_printf_info("MVEC: playing vector stream from %s\n", playback_path);
			// The overlay leaves no other trace in a log, and this path is driven from scripts.
			osd_printf_info("MVEC: position overlay %s, end of stream: %s\n",
					m_tool_overlay ? "shown" : "hidden",
					(m_owner.machine().options().vector_playback_end() == 1) ? "exit"
						: (m_owner.machine().options().vector_playback_end() == 2) ? "loop" : "hold");
		}
	}

	~stream_state()
	{
		finish();
		release_claim();
	}

	mode current_mode() const { return m_mode; }
	bool recording() const { return m_mode == mode::RECORD; }
	bool playing() const { return m_mode == mode::PLAYBACK; }
	bool playback_paused() const { return m_tool_paused; }
	bool playback_advanced() const { return m_playback_advanced; }
	attotime playback_frame_period() const
	{
		return (m_recorded_frame_period > 0)
			? attotime(0, m_recorded_frame_period)
			: m_owner.screen().frame_period();
	}
	void sync_playback_audio()
	{
		const bool discontinuity = !m_audio_sync_valid || m_audio_reset != m_playback_reset || m_audio_paused != m_tool_paused;
		const double time_seconds = double(playback_position()) * playback_frame_period().as_double();
		m_owner.machine().sound().vector_playback_sync(time_seconds, m_tool_paused, discontinuity);
		m_audio_sync_valid = true;
		m_audio_reset = m_playback_reset;
		m_audio_paused = m_tool_paused;
	}
	u32 playback_reset() const { return m_playback_reset; }
	u64 playback_position() const { return m_play_position < 0 ? 0U : u64(m_play_position); }
	u64 playback_total() const { return u64(m_frame_index.size()); }

	void finish()
	{
		if (m_mode == mode::RECORD)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_stopping = true;
			}
			m_not_empty.notify_all();
			m_not_full.notify_all();
			if (m_writer.joinable())
				m_writer.join();
			m_output.flush();
			m_output.close();
			if (m_write_failed)
				osd_printf_error("MVEC: recording failed after %llu frames: %s\n", (unsigned long long)m_frames, m_write_error);
			else
				osd_printf_info("MVEC: recorded %llu frames\n", (unsigned long long)m_frames);
		}
		else if (m_mode == mode::PLAYBACK)
		{
			m_input.close();
			m_owner.machine().sound().vector_playback_mute(false);
		}
		m_mode = mode::NONE;
	}

	void record_frame(const point *points, int count, bool stale, bool timed, u32 generation, const rectangle &visarea)
	{
		if (!recording()) return;
		std::vector<u8> payload;
		const u32 stored_count = stale ? 0U : u32(count);
		payload.reserve(40U + size_t(stored_count) * 72U);
		append_u64(payload, m_frames);
		append_u32(payload, (stale ? 1U : 0U) | (timed ? 2U : 0U));
		append_u32(payload, generation);
		append_i32(payload, visarea.min_x); append_i32(payload, visarea.max_x);
		append_i32(payload, visarea.min_y); append_i32(payload, visarea.max_y);
		append_u32(payload, stored_count);
		for (u32 i = 0; i < stored_count; ++i)
		{
			const point &p = points[i];
			append_i32(payload, p.x0); append_i32(payload, p.y0);
			append_i32(payload, p.x); append_i32(payload, p.y);
			append_u32(payload, u32(p.col));
			append_u8(payload, u8(p.intensity));
			append_float(payload, p.beam_energy);
			append_i32(payload, p.t0.seconds()); append_i64(payload, p.t0.attoseconds());
			append_i32(payload, p.t1.seconds()); append_i64(payload, p.t1.attoseconds());
			append_u32(payload, p.cap_flags);
			append_i32(payload, p.dump_scale);
			append_double(payload, p.dump_ramp_us);
			append_u8(payload, p.dump_midchange ? 1U : 0U);
		}

		std::vector<u8> chunk;
		chunk.reserve(payload.size() + 12U);
		append_u32(chunk, MVEC_FRAME_MAGIC);
		append_u32(chunk, u32(payload.size()));
		chunk.insert(chunk.end(), payload.begin(), payload.end());
		append_u32(chunk, u32(util::crc32_creator::simple(payload.data(), u32(payload.size()))));
		enqueue(std::move(chunk));
		++m_frames;
	}

	bool playback_frame(point *dest, int capacity, int &count, bool &stale, bool &timed, u32 &generation, rectangle &visarea)
	{
		if (!playing()) return false;
		m_playback_advanced = false;
		poll_playback_tool();
		if (!m_tool_announced)
		{
			m_tool_announced = true;
			show_tool_message("Ready");
		}

		u64 target = (m_pending_position != INVALID_POSITION)
			? m_pending_position
			: (m_play_position < 0 ? 0U : u64(m_play_position) + (m_tool_paused ? 0U : 1U));
		m_pending_position = INVALID_POSITION;
		if (target >= m_frame_index.size())
		{
			// vector_playback_end 2 wraps instead of stopping. Falling through with target 0 lets the
			// normal seek path below do the work: it sees a position that is not the previous one plus
			// one, so it reloads the frame and raises the discontinuity that resynchronises the audio.
			if ((m_owner.machine().options().vector_playback_end() == 2) && !m_frame_index.empty())
			{
				target = 0;
				m_eof = false;
				show_tool_message("Looped");
			}
			else
			{
				if (!m_eof)
					playback_end("End of file");
				copy_cached(dest, capacity, count, true);
				stale = true;
				if (m_tool_overlay)
					show_tool_message("End of file");
				return true;
			}
		}

		if (m_play_position < 0 || target != u64(m_play_position))
		{
			const bool discontinuity = m_play_position >= 0 && target != u64(m_play_position) + 1U;
			read_indexed_frame(target, stale, timed, generation, visarea);
			m_play_position = s64(target);
			m_playback_advanced = true;
			m_eof = false;
			if (discontinuity)
				++m_playback_reset;
		}
		else
		{
			const frame_index_entry &entry = m_frame_index[target];
			stale = entry.stale;
			timed = entry.timed;
			generation = entry.generation;
			visarea = entry.visarea;
		}
		copy_cached(dest, capacity, count, stale);
		if (m_tool_overlay)
			show_tool_message(m_tool_paused ? "Paused" : "Playing");
		return true;
	}

private:
	void release_claim()
	{
		if (m_claimed)
		{
			std::lock_guard<std::mutex> lock(s_mvec_claim_mutex);
			s_mvec_claimed = false;
			m_claimed = false;
		}
	}

	static constexpr u64 INVALID_POSITION = std::numeric_limits<u64>::max();
	struct frame_index_entry
	{
		std::streamoff offset = 0;
		bool stale = false;
		bool timed = false;
		u32 generation = 0;
		u32 point_count = 0;
		rectangle visarea;
		u64 content_frame = INVALID_POSITION;
	};

	void read_indexed_frame(u64 index, bool &stale, bool &timed, u32 &generation, rectangle &visarea)
	{
		const frame_index_entry &entry = m_frame_index[index];
		if (entry.stale && entry.content_frame != INVALID_POSITION)
		{
			bool source_stale, source_timed;
			u32 source_generation;
			rectangle source_visarea;
			decode_frame(entry.content_frame, source_stale, source_timed, source_generation, source_visarea);
		}
		decode_frame(index, stale, timed, generation, visarea);
	}

	void decode_frame(u64 expected_index, bool &stale, bool &timed, u32 &generation, rectangle &visarea)
	{
		m_input.clear();
		m_input.seekg(m_frame_index[expected_index].offset);
		if (!m_input)
			fatalerror("MVEC seek failed at frame %llu\n", (unsigned long long)expected_index);
		u32 magic;
		if (!read_file_u32(magic, false) || magic != MVEC_FRAME_MAGIC)
			fatalerror("MVEC frame marker is invalid at frame %llu\n", (unsigned long long)expected_index);
		u32 payload_size;
		if (!read_file_u32(payload_size, false) || payload_size > 128U * 1024U * 1024U)
			fatalerror("MVEC frame size is invalid\n");
		std::vector<u8> payload(payload_size);
		if (!read_exact(payload.data(), payload.size()))
			fatalerror("MVEC frame payload is truncated\n");
		u32 stored_crc;
		if (!read_file_u32(stored_crc, false))
			fatalerror("MVEC frame checksum is missing\n");
		const u32 actual_crc = u32(util::crc32_creator::simple(payload.data(), u32(payload.size())));
		if (stored_crc != actual_crc)
			fatalerror("MVEC CRC mismatch at frame %llu\n", (unsigned long long)expected_index);

		payload_reader reader(payload);
		const u64 frame_number = reader.get_u64();
		if (frame_number != expected_index)
			fatalerror("MVEC frame sequence mismatch (expected %llu, got %llu)\n", (unsigned long long)expected_index, (unsigned long long)frame_number);
		const u32 flags = reader.get_u32();
		stale = bool(flags & 1U);
		timed = bool(flags & 2U);
		generation = reader.get_u32();
		visarea.min_x = reader.get_i32(); visarea.max_x = reader.get_i32();
		visarea.min_y = reader.get_i32(); visarea.max_y = reader.get_i32();
		const u32 stored_count = reader.get_u32();
		if (stored_count > MAX_POINTS)
			fatalerror("MVEC frame has too many points (%u)\n", stored_count);
		if (stale && stored_count != 0)
			fatalerror("MVEC stale frame unexpectedly contains points\n");
		if (!stale)
		{
			m_cached.resize(stored_count);
			for (point &p : m_cached)
			{
				p.x0 = reader.get_i32(); p.y0 = reader.get_i32();
				p.x = reader.get_i32(); p.y = reader.get_i32();
				p.col = rgb_t(reader.get_u32());
				p.intensity = reader.get_u8();
				p.beam_energy = reader.get_float();
				const s32 t0s = reader.get_i32(); const s64 t0a = reader.get_i64();
				const s32 t1s = reader.get_i32(); const s64 t1a = reader.get_i64();
				p.t0 = attotime(t0s, t0a); p.t1 = attotime(t1s, t1a);
				p.cap_flags = reader.get_u32();
				p.dump_scale = reader.get_i32();
				p.dump_ramp_us = reader.get_double();
				p.dump_midchange = reader.get_u8() != 0;
				p.emitted = false;
			}
		}
		if (!reader.done())
			fatalerror("MVEC frame contains unsupported trailing data\n");
	}
	void write_header()
	{
		std::vector<u8> header;
		const char magic[8] = { 'M','A','M','E','V','E','C','\0' };
		header.insert(header.end(), magic, magic + sizeof(magic));
		append_u16(header, MVEC_VERSION_MAJOR); append_u16(header, MVEC_VERSION_MINOR);
		append_u32(header, 0x12345678U);
		append_string(header, m_owner.machine().system().name);
		append_string(header, m_owner.tag());
		append_i64(header, m_recorded_frame_period);
		m_output.write(reinterpret_cast<const char *>(header.data()), std::streamsize(header.size()));
		if (!m_output) fatalerror("Unable to write MVEC header\n");
	}

	void read_header()
	{
		char magic[8];
		if (!read_exact(magic, sizeof(magic)) || std::memcmp(magic, "MAMEVEC\0", 8))
			fatalerror("MVEC file is corrupt or unsupported\n");
		u16 major, minor; u32 endian;
		if (!read_file_u16(major) || !read_file_u16(minor) || !read_file_u32(endian, false))
			fatalerror("MVEC header is truncated\n");
		if (major != MVEC_VERSION_MAJOR || minor > MVEC_VERSION_MINOR || endian != 0x12345678U)
			fatalerror("MVEC format version or byte order is unsupported\n");
		const std::string system = read_file_string();
		const std::string device = read_file_string();
		if (minor >= 1 && !read_file_i64(m_recorded_frame_period))
			fatalerror("MVEC recorded frame period is truncated\n");
		if (system != m_owner.machine().system().name)
			fatalerror("MVEC file is for machine '%s', not '%s'\n", system.c_str(), m_owner.machine().system().name);
		if (device != m_owner.tag())
			osd_printf_warning("MVEC: recorded device is '%s', current device is '%s'\n", device.c_str(), m_owner.tag());
		osd_printf_info("MVEC format %u.%u, machine %s, device %s\n", major, minor, system.c_str(), device.c_str());
		build_frame_index();
		// -vector_playback_start puts the stream where the goto tool would, before the first frame is
		// served. Investigating something that only happens thousands of frames in otherwise means
		// typing the position by hand every run, which is not a repeatable measurement.
		{
			const int start = m_owner.machine().options().vector_playback_start();
			if (start > 0 && !m_frame_index.empty())
			{
				// The option counts frames the way the overlay and the goto tool do, from 1, so a
				// number read off the overlay can be pasted straight into the command line.
				m_pending_position = std::min<u64>(u64(start) - 1U, m_frame_index.size() - 1U);
				osd_printf_info("MVEC: starting playback at frame %llu\n",
					(unsigned long long)(m_pending_position + 1U));
			}
		}
		if (m_recorded_frame_period <= 0)
			m_recorded_frame_period = infer_frame_period();
		if (m_recorded_frame_period <= 0)
		{
			m_recorded_frame_period = m_owner.screen().frame_period().attoseconds();
			osd_printf_warning("MVEC: no recorded frame rate; using current screen rate %.6f Hz\n",
				playback_frame_period().as_hz());
		}
		else
		{
			osd_printf_info("MVEC: playback source rate %.6f Hz\n", playback_frame_period().as_hz());
		}
	}

	void build_frame_index()
	{
		u64 last_content = INVALID_POSITION;
		for (u64 expected = 0; ; ++expected)
		{
			const std::streamoff offset = m_input.tellg();
			u32 magic;
			if (!read_file_u32(magic, true))
				break;
			if (magic != MVEC_FRAME_MAGIC)
				fatalerror("MVEC frame marker is invalid at frame %llu\n", (unsigned long long)expected);
			u32 payload_size;
			if (!read_file_u32(payload_size, false) || payload_size > 128U * 1024U * 1024U)
				fatalerror("MVEC frame size is invalid\n");
			std::vector<u8> payload(payload_size);
			if (!read_exact(payload.data(), payload.size()))
				fatalerror("MVEC frame payload is truncated\n");
			u32 stored_crc;
			if (!read_file_u32(stored_crc, false))
				fatalerror("MVEC frame checksum is missing\n");
			if (stored_crc != u32(util::crc32_creator::simple(payload.data(), u32(payload.size()))))
				fatalerror("MVEC CRC mismatch at frame %llu\n", (unsigned long long)expected);

			payload_reader reader(payload);
			const u64 number = reader.get_u64();
			if (number != expected)
				fatalerror("MVEC frame sequence mismatch while indexing\n");
			const u32 flags = reader.get_u32();
			frame_index_entry entry;
			entry.offset = offset;
			entry.stale = bool(flags & 1U);
			entry.timed = bool(flags & 2U);
			entry.generation = reader.get_u32();
			entry.visarea.min_x = reader.get_i32(); entry.visarea.max_x = reader.get_i32();
			entry.visarea.min_y = reader.get_i32(); entry.visarea.max_y = reader.get_i32();
			const u32 count = reader.get_u32();
			entry.point_count = count;
			if (count > MAX_POINTS || payload_size != 36U + count * 66U)
				fatalerror("MVEC frame payload size is invalid at frame %llu\n", (unsigned long long)expected);
			if (entry.stale)
			{
				if (count)
					fatalerror("MVEC stale frame unexpectedly contains points\n");
				entry.content_frame = last_content;
			}
			else
			{
				entry.content_frame = expected;
				last_content = expected;
			}
			m_frame_index.emplace_back(entry);
		}
		m_input.clear();
		osd_printf_info("MVEC: indexed %llu frames\n", (unsigned long long)m_frame_index.size());
		if (m_frame_index.empty())
			fatalerror("MVEC file contains no frames\n");
	}

	attoseconds_t infer_frame_period()
	{
		// Version 1.0 did not store a frame period. Timed vector streams still contain absolute t0
		// values, so recover it from how far the first point's timestamp advances across the stream.
		// Seek directly to that timestamp in a bounded sample of indexed frames; do not reread
		// multi-gigabyte payloads.
		//
		// The estimator is the total span over the frames it covers, NOT the median inter-frame
		// advance. A list-start interval is not a fixed quantity: the game restarts the AVG, and a
		// heavy frame takes longer, so the distribution is skewed right - on starwars.mvec the
		// advances run 12.2 ms at the 5th percentile to 24.5 ms at the 95th. Its median therefore
		// reads well below the true average, by 2.5% on that stream and by 13% on sw_mglow.mvec.
		// Playback consumes one recorded frame per screen update, so the average is what matters.
		//
		// Samples are still screened pairwise: an implied period outside [1 ms, 1 s] means the newer
		// timestamp is not trustworthy, so it is dropped rather than becoming an endpoint of the span.
		const size_t stride = std::max<size_t>(1, m_frame_index.size() / 4096);
		long double previous_time = 0.0L;
		u64 previous_frame = 0;
		long double first_time = 0.0L;
		u64 first_frame = 0;
		unsigned accepted = 0;
		bool have_previous = false;
		for (size_t i = 0; i < m_frame_index.size(); i += stride)
		{
			const frame_index_entry &entry = m_frame_index[i];
			if (entry.stale || !entry.point_count)
				continue;

			// frame marker/size (8), payload header (36), point fields before t0 (25)
			m_input.clear();
			m_input.seekg(entry.offset + std::streamoff(69));
			u8 bytes[12];
			if (!read_exact(bytes, sizeof(bytes)))
				continue;
			auto get_u32 = [](const u8 *p) -> u32
			{
				return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
			};
			auto get_u64 = [&](const u8 *p) -> u64
			{
				return u64(get_u32(p)) | (u64(get_u32(p + 4)) << 32);
			};
			const s32 seconds = s32(get_u32(bytes));
			const s64 attoseconds = s64(get_u64(bytes + 4));
			if (seconds < 0 || seconds >= 100000000 || attoseconds < 0 || attoseconds >= ATTOSECONDS_PER_SECOND)
				continue;
			const long double current_time = static_cast<long double>(seconds)
				+ static_cast<long double>(attoseconds) / static_cast<long double>(ATTOSECONDS_PER_SECOND);
			if (!have_previous)
			{
				first_time = current_time;
				first_frame = u64(i);
				previous_time = current_time;
				previous_frame = u64(i);
				have_previous = true;
				accepted = 1;
				continue;
			}
			if (current_time <= previous_time)
				continue;
			const double implied = double((current_time - previous_time)
				/ static_cast<long double>(u64(i) - previous_frame));
			if (implied < 0.001 || implied > 1.0)
				continue;
			previous_time = current_time;
			previous_frame = u64(i);
			++accepted;
		}
		m_input.clear();
		if (accepted < 2 || previous_frame <= first_frame)
			return 0;
		const double mean = double((previous_time - first_time)
			/ static_cast<long double>(previous_frame - first_frame));
		if (mean < 0.001 || mean > 1.0)
			return 0;
		const attoseconds_t result = attoseconds_t(std::llround(mean * double(ATTOSECONDS_PER_SECOND)));
		osd_printf_info("MVEC: inferred legacy source rate %.6f Hz from timed beam events across frames %llu to %llu\n",
				1.0 / mean, (unsigned long long)first_frame, (unsigned long long)previous_frame);
		return result;
	}

	bool read_exact(void *dest, size_t size)
	{
		m_input.read(reinterpret_cast<char *>(dest), std::streamsize(size));
		return size_t(m_input.gcount()) == size;
	}
	bool read_file_u16(u16 &value)
	{
		u8 b[2]; if (!read_exact(b, sizeof(b))) return false; value = u16(b[0]) | (u16(b[1]) << 8); return true;
	}
	bool read_file_u32(u32 &value, bool allow_clean_eof)
	{
		u8 b[4];
		m_input.read(reinterpret_cast<char *>(b), sizeof(b));
		const std::streamsize got = m_input.gcount();
		if (got == 0 && allow_clean_eof) return false;
		if (got != sizeof(b)) return false;
		value = u32(b[0]) | (u32(b[1]) << 8) | (u32(b[2]) << 16) | (u32(b[3]) << 24); return true;
	}
	bool read_file_i64(s64 &value)
	{
		u8 b[8];
		if (!read_exact(b, sizeof(b)))
			return false;
		u64 raw = 0;
		for (unsigned shift = 0; shift < 64; shift += 8)
			raw |= u64(b[shift / 8]) << shift;
		value = s64(raw);
		return true;
	}
	std::string read_file_string()
	{
		u16 size; if (!read_file_u16(size)) fatalerror("MVEC header string is truncated\n");
		std::string result(size, '\0');
		if (size && !read_exact(result.data(), size)) fatalerror("MVEC header string is truncated\n");
		return result;
	}

	void poll_playback_tool()
	{
		input_manager &input = m_owner.machine().input();
		if (m_goto_mode)
		{
			static const input_code digit_codes[10] = {
				KEYCODE_0, KEYCODE_1, KEYCODE_2, KEYCODE_3, KEYCODE_4,
				KEYCODE_5, KEYCODE_6, KEYCODE_7, KEYCODE_8, KEYCODE_9 };
			for (int digit = 0; digit < 10; ++digit)
				if (input.code_pressed_once(digit_codes[digit]) && m_goto_digits.size() < 19)
					m_goto_digits += char('0' + digit);
			if (input.code_pressed_once(KEYCODE_BACKSPACE) && !m_goto_digits.empty())
				m_goto_digits.pop_back();
			if (input.code_pressed_once(KEYCODE_ESC))
			{
				m_goto_mode = false;
				show_tool_message("Go to cancelled");
				return;
			}
			if (input.code_pressed_once(KEYCODE_ENTER) || input.code_pressed_once(KEYCODE_ENTER_PAD))
			{
				if (!m_goto_digits.empty())
				{
					u64 value = 0;
					for (char c : m_goto_digits)
						value = std::min<u64>(INVALID_POSITION / 10U, value) * 10U + u64(c - '0');
					request_position(value ? value - 1U : 0U, true);
				}
				m_goto_mode = false;
				m_goto_digits.clear();
				show_tool_message("Go to");
				return;
			}
			show_tool_message("Enter frame number");
			return;
		}

		const bool alt = input.code_pressed(KEYCODE_LALT) || input.code_pressed(KEYCODE_RALT);
		// Use an explicit chord edge for Alt+O. code_pressed_once() has shared switch memory and can
		// be sampled elsewhere in the UI/input stack; keeping our own edge makes the toggle deterministic.
		const bool overlay_chord = alt && input.code_pressed(KEYCODE_O);
		const bool overlay_pressed = overlay_chord && !m_overlay_key_down;
		m_overlay_key_down = overlay_chord;
		if (overlay_pressed)
		{
			m_tool_overlay = !m_tool_overlay;
			if (m_tool_overlay)
				show_tool_message(m_tool_paused ? "Paused" : "Playing");
			else
				m_owner.machine().ui().set_vector_playback_text(std::string());
			return;
		}
		if (!alt)
			return;
		if (input.code_pressed_once(KEYCODE_P))
		{
			m_tool_paused = !m_tool_paused;
			show_tool_message(m_tool_paused ? "Paused" : "Playing");
		}
		else if (input.code_pressed_once(KEYCODE_LEFT)) request_relative(-1);
		else if (input.code_pressed_once(KEYCODE_RIGHT)) request_relative(1);
		else if (input.code_pressed_once(KEYCODE_PGUP)) request_relative(-60);
		else if (input.code_pressed_once(KEYCODE_PGDN)) request_relative(60);
		else if (input.code_pressed_once(KEYCODE_HOME)) request_position(0, true);
		else if (input.code_pressed_once(KEYCODE_END)) request_position(m_frame_index.size() - 1U, true);
		else if (input.code_pressed_once(KEYCODE_G))
		{
			// A hidden overlay makes the modal frame-number input indistinguishable
			// from an input lock. Entering go-to mode always makes its prompt visible;
			// Alt+O remains the explicit way to hide it again afterwards.
			m_tool_overlay = true;
			m_goto_mode = true;
			m_goto_digits.clear();
			m_tool_paused = true;
			show_tool_message("Enter frame number");
		}

	}

	void request_relative(s64 delta)
	{
		const s64 base = std::max<s64>(m_play_position, 0);
		const u64 target = u64(std::clamp<s64>(base + delta, 0, s64(m_frame_index.size() - 1U)));
		request_position(target, true);
		show_tool_message(delta < 0 ? "Step back" : "Step forward");
	}

	void request_position(u64 target, bool pause)
	{
		m_pending_position = std::min<u64>(target, m_frame_index.size() - 1U);
		if (pause)
			m_tool_paused = true;
		m_eof = false;
	}

	void show_tool_message(const char *status)
	{
		// Alt+O is authoritative: navigation and EOF status must never resurrect a hidden overlay.
		if (!m_tool_overlay)
			return;
#if defined(__APPLE__)
		const char *const modifier_name = "Option";
#else
		const char *const modifier_name = "Alt";
#endif
		const u64 shown = (m_pending_position != INVALID_POSITION ? m_pending_position : playback_position()) + 1U;
		std::string text;
		if (m_goto_mode)
			text = util::string_format(
				"MVEC playback: %s\nFrame: %s / %llu\nEnter: jump  Backspace: erase  Esc: cancel",
				status, m_goto_digits.empty() ? "_" : m_goto_digits.c_str(),
				(unsigned long long)m_frame_index.size());
		else
			text = util::string_format(
				"MVEC playback: %s\nFrame: %llu / %llu\n%s+P play/pause  %s+Left/Right step\n"
				"%s+PgUp/PgDn 60 frames  %s+Home/End  %s+G go to  %s+O overlay",
				status, (unsigned long long)shown, (unsigned long long)m_frame_index.size(),
				modifier_name, modifier_name, modifier_name, modifier_name, modifier_name, modifier_name);
		m_owner.machine().ui().set_vector_playback_text(std::move(text));
	}
	void enqueue(std::vector<u8> &&chunk)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_not_full.wait(lock, [&] { return m_write_failed || m_queued_bytes + chunk.size() <= MVEC_QUEUE_LIMIT || m_queue.empty(); });
		if (m_write_failed) fatalerror("MVEC writer failed: %s\n", m_write_error);
		m_queued_bytes += chunk.size();
		m_queue.emplace_back(std::move(chunk));
		lock.unlock();
		m_not_empty.notify_one();
	}

	void writer_loop()
	{
		for (;;)
		{
			std::vector<u8> chunk;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_not_empty.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
				if (m_queue.empty())
				{
					if (m_stopping) break;
					continue;
				}
				chunk = std::move(m_queue.front());
				m_queue.pop_front();
				m_queued_bytes -= chunk.size();
			}
			m_not_full.notify_one();
			m_output.write(reinterpret_cast<const char *>(chunk.data()), std::streamsize(chunk.size()));
			if (!m_output)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_write_failed = true;
				m_write_error = "disk write error";
				m_queue.clear(); m_queued_bytes = 0;
				m_not_full.notify_all();
				break;
			}
		}
	}

	void copy_cached(point *dest, int capacity, int &count, bool stale)
	{
		if (m_cached.size() > size_t(capacity)) fatalerror("MVEC playback buffer overflow\n");
		count = int(m_cached.size());
		for (int i = 0; i < count; ++i)
		{
			dest[i] = m_cached[i];
			dest[i].emitted = stale;
		}
	}

	void playback_end(const char *reason)
	{
		m_eof = true;
		m_tool_paused = true;
		osd_printf_info("MVEC: playback ended after %llu frames (%s)\n", (unsigned long long)m_frame_index.size(), reason);
		show_tool_message("End of file");
		if (m_owner.machine().options().vector_playback_end() == 1)
			m_owner.machine().schedule_exit();
	}

	vector_device &m_owner;
	mode m_mode = mode::NONE;
	bool m_claimed = false;
	std::ofstream m_output;
	std::ifstream m_input;
	std::thread m_writer;
	std::mutex m_mutex;
	std::condition_variable m_not_empty;
	std::condition_variable m_not_full;
	std::deque<std::vector<u8>> m_queue;
	size_t m_queued_bytes = 0;
	bool m_stopping = false;
	bool m_write_failed = false;
	const char *m_write_error = nullptr;
	bool m_eof = false;
	u64 m_frames = 0;
	std::vector<point> m_cached;
	std::vector<frame_index_entry> m_frame_index;
	s64 m_play_position = -1;
	u64 m_pending_position = INVALID_POSITION;
	bool m_tool_paused = false;
	bool m_playback_advanced = false;
	bool m_audio_sync_valid = true;
	bool m_audio_paused = false;
	u32 m_audio_reset = 0;
	bool m_tool_announced = false;
	bool m_tool_overlay = true;
	bool m_overlay_key_down = false;
	bool m_goto_mode = false;
	std::string m_goto_digits;
	u32 m_playback_reset = 0;
	attoseconds_t m_recorded_frame_period = 0;
};

float vector_options::s_flicker = 0.0f;
float vector_options::s_beam_width_min = 0.0f;
float vector_options::s_beam_width_max = 0.0f;
float vector_options::s_beam_dot_size = 0.0f;
float vector_options::s_beam_intensity_weight = 0.0f;
float vector_options::s_overscan_x = 1.0f;
float vector_options::s_overscan_y = 1.0f;
float vector_options::s_blank_leak = 0.0f;
float vector_options::s_window_droop = 0.0f;
float vector_options::s_window_memory = 0.0f;
float vector_options::s_window_jitter = 0.0f;
float vector_options::s_window_bias = 0.0f;

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
	// The scatter is the only one of the four that moves between frames, so it is the one that reads
	// as the window wobbling, and -vector_window_scatter is its switch. The other three shift the window
	// by a fixed amount or lean it along the drawing order; they do not move, so they are always in
	// and carry the calibrated values. Scatter is off by default because Battlezone holds all four
	// edges of the rectangle and 1% of screen height on each of them looks like the frame breathing,
	// which is a different thing from Major Havoc's single trim line moving.
	s_window_droop = options.vector_window_droop();
	s_window_memory = options.vector_window_memory();
	s_window_jitter = options.vector_window_scatter() ? options.vector_window_jitter() : 0.0f;
	s_window_bias = options.vector_window_bias();
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
vector_device::~vector_device() = default;


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

	// MVEC capture/playback is independent of the legacy analysis CSV dump.
	m_stream = std::make_unique<stream_state>(*this);
	if (m_stream->playing())
	{
		const attotime period = m_stream->playback_frame_period();
		if (period != screen().frame_period())
		{
			osd_printf_info("MVEC: configuring vector screen from %.6f Hz to recorded %.6f Hz\n",
				screen().frame_period().as_hz(), period.as_hz());
			screen().configure(screen().width(), screen().height(), screen().visible_area(), period.attoseconds());
		}
	}
}

void vector_device::device_stop()
{
	m_stream.reset();
	if (m_event_dump.is_open())
		m_event_dump.close();
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
	newpoint->dump_scale = m_dump_scale;
	newpoint->dump_ramp_us = m_dump_ramp_us;
	newpoint->dump_midchange = m_dump_midchange;
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

void vector_device::clear_list(bool advance_generation)
{
	m_vector_index = 0;
	// A new beam list is starting; bump the generation so screen_update can tell this frame redrew.
	if (advance_generation)
		m_list_generation++;
}

//-------------------------------------------------
// Update the screen container with queued vectors.
//-------------------------------------------------

uint32_t vector_device::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	uint32_t flags = PRIMFLAG_ANTIALIAS(1) | PRIMFLAG_BLENDMODE(BLENDMODE_ADD) | PRIMFLAG_VECTOR(1);
	rectangle visarea = screen.visible_area();
	bool playback_frame = false;
	bool playback_stale = false;
	bool frame_timed = m_avg_timing;
	if (m_stream && m_stream->playing())
	{
		int playback_count = 0;
		u32 playback_generation = 0;
		playback_frame = m_stream->playback_frame(m_vector_list.get(), MAX_POINTS, playback_count,
			playback_stale, frame_timed, playback_generation, visarea);
		m_vector_index = playback_count;
		m_list_generation = playback_generation;
		m_stream->sync_playback_audio();
		m_min_intensity = 255;
		m_max_intensity = 0;
		for (int i = 0; i < m_vector_index; ++i)
		{
			const int intensity = m_vector_list[i].intensity;
			if (intensity > 0)
			{
				m_min_intensity = std::min(m_min_intensity, intensity);
				m_max_intensity = std::max(m_max_intensity, intensity);
			}
		}
	}

	float xscale = 1.0f / (65536 * visarea.width());
	float yscale = 1.0f / (65536 * visarea.height());
	float xoffs = (float)visarea.min_x;
	float yoffs = (float)visarea.min_y;

	point *curpoint = m_vector_list.get();
	screen.container().empty();
	screen.container().add_rect(0.0f, 0.0f, 1.0f, 1.0f, rgb_t(0xff,0x00,0x00,0x00), PRIMFLAG_BLENDMODE(BLENDMODE_ALPHA) | PRIMFLAG_VECTORBUF(1));

	m_frame_begin_notifier();

	// CRT-flicker detection normally follows list generations. MVEC playback restores the recorded
	// stale decision explicitly because it affects notifiers, EHT load and temporal renderer state.
	m_beam_list_stale = playback_frame ? playback_stale : (m_list_generation == m_last_drawn_generation);
	m_last_drawn_generation = m_list_generation;

	if (m_stream && m_stream->recording())
		m_stream->record_frame(m_vector_list.get(), m_vector_index, m_beam_list_stale,
			frame_timed, m_list_generation, visarea);

	// Per-frame statistics for the render container (see render_vector_stats): total beam energy
	// (EHT load) and shaped off-screen energy (monitor glow), accumulated below once per beam
	// event (window-boundary re-emissions do not recount, matching the notifiers).
	float stats_total_energy = 0.0f;
	float stats_offscreen_energy[render_vector_stats::OFFSCREEN_DEPTH_BINS] = {};
	float stats_monitor_glow_coverage[render_vector_stats::MONITOR_GLOW_ANGLE_BINS][render_vector_stats::OFFSCREEN_DEPTH_BINS] = {};
	float stats_edge_energy[4][render_vector_stats::EDGE_GLOW_BINS] = {};
	// Sweep extent of this list (see render_vector_stats::sweep_t0). Blanked moves count: the beam
	// is physically travelling during them, so they are part of the pass's sweep time.
	double stats_sweep_t0 = -1.0, stats_sweep_t1 = -1.0;

	for (int i = 0; i < m_vector_index; i++)
	{
		render_bounds coords;

		// Converted once here and reused by both add_line branches below.
		const double prim_t0 = curpoint->t0.is_never() ? -1.0 : curpoint->t0.as_double();
		const double prim_t1 = curpoint->t1.is_never() ? -1.0 : curpoint->t1.as_double();
		if (prim_t0 >= 0.0 && prim_t1 > prim_t0)
		{
			if (stats_sweep_t0 < 0.0 || prim_t0 < stats_sweep_t0)
				stats_sweep_t0 = prim_t0;
			if (prim_t1 > stats_sweep_t1)
				stats_sweep_t1 = prim_t1;
		}

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
		const render_bounds monitor_coords = coords;

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
			// A zero-length lit event can either be a deliberately parked, stand-alone dot
			// (bullet/star) or a brief Z pause at a line junction.  Point-only optical effects
			// such as halation and starburst belong only to the former.  Preserve the core dot
			// in both cases, but tag a point touching either adjacent visible segment so the
			// renderer can suppress only its optical geometry.  Deriving this here also works
			// for old MVEC recordings, which predate the metadata bit.
			u32 cap_flags = curpoint->cap_flags;
			if (curpoint->x0 == curpoint->x && curpoint->y0 == curpoint->y)
			{
				const point *const prev = (i > 0) ? &m_vector_list[i - 1] : nullptr;
				const point *const next = (i + 1 < m_vector_index) ? &m_vector_list[i + 1] : nullptr;
				const bool joins_prev = prev && prev->intensity != 0
					&& prev->x == curpoint->x && prev->y == curpoint->y;
				const bool joins_next = next && next->intensity != 0
					&& next->x0 == curpoint->x && next->y0 == curpoint->y;
				if (joins_prev || joins_next)
					cap_flags |= VECTOR_CAP_POINT_OPTICS_SUPPRESS;
			}
			screen.container().add_line(
					coords.x0, coords.y0, coords.x1, coords.y1,
					beam_width,
					(curpoint->intensity << 24) | (curpoint->col & 0xffffff),
					flags,
					curpoint->beam_energy,
					prim_t0,
					prim_t1,
					cap_flags);
			// Points surviving into a second emission (window-boundary blend) re-emit their
			// primitive but must not re-fire the notifiers: one beam event, one notification.
			if (!curpoint->emitted)
			{
				m_line_notifier(curpoint->x0, curpoint->y0, curpoint->x, curpoint->y, curpoint->col, curpoint->intensity, visarea.width(), visarea.height());
				// Parallel notifier: normalized-space endpoints + beam energy, for off-screen beam effects.
				m_beam_energy_line_notifier(coords.x0, coords.y0, coords.x1, coords.y1, curpoint->beam_energy);

				if (curpoint->beam_energy > 0.0f)
				{
					// Monitor glow describes physical deflection, not the optional display zoom.
					// Use the normalized coordinates captured before vector_overscan_x/y.
					accumulate_monitor_glow_coverage(monitor_coords, curpoint->beam_energy,
							stats_monitor_glow_coverage);
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
						prim_t0,
						prim_t1,
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
	const bool playback_active = m_stream && m_stream->playing();
	const bool playback_advanced = !playback_active || m_stream->playback_advanced();
	if (playback_advanced)
		++m_stats_frame_id;
	stats.frame_id = m_stats_frame_id;
	stats.list_generation = m_list_generation;
	stats.list_stale = m_beam_list_stale;
	stats.timed = frame_timed;
	stats.sweep_t0 = stats_sweep_t0;
	stats.sweep_t1 = stats_sweep_t1;
	stats.total_energy = stats_total_energy;
	stats.playback_active = playback_active;
	stats.playback_paused = playback_active && m_stream->playback_paused();
	const attotime playback_period = playback_active
		? m_stream->playback_frame_period() : screen.frame_period();
	stats.playback_dt_ms = (playback_active && playback_advanced)
		? float(playback_period.as_double() * 1000.0) : 0.0f;
	stats.playback_time_ms = playback_active
		? double(m_stream->playback_position()) * playback_period.as_double() * 1000.0 : 0.0;
	stats.playback_reset = playback_active ? m_stream->playback_reset() : 0U;
	stats.playback_position = playback_active ? m_stream->playback_position() : 0U;
	stats.playback_total = playback_active ? m_stream->playback_total() : 0U;
	static_assert(sizeof(stats.offscreen_energy) == sizeof(stats_offscreen_energy));
	memcpy(stats.offscreen_energy, stats_offscreen_energy, sizeof(stats.offscreen_energy));
	static_assert(sizeof(stats.monitor_glow_coverage) == sizeof(stats_monitor_glow_coverage));
	memcpy(stats.monitor_glow_coverage, stats_monitor_glow_coverage, sizeof(stats.monitor_glow_coverage));
	static_assert(sizeof(stats.edge_energy) == sizeof(stats_edge_energy));
	memcpy(stats.edge_energy, stats_edge_energy, sizeof(stats.edge_energy));
	screen.container().set_vector_stats(stats);

	return 0;
}
