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

#include "util/hashing.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
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
constexpr u16 MVEC_VERSION_MINOR = 0;
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
			fatalerror("-vector_record and -vector_playback are mutually exclusive\n");
		if (!record && !playback)
			return;

		{
			std::lock_guard<std::mutex> lock(s_mvec_claim_mutex);
			if (s_mvec_claimed)
				fatalerror("MVEC currently supports one vector device per machine\n");
			s_mvec_claimed = true;
			m_claimed = true;
		}

		if (record)
		{
			m_mode = mode::RECORD;
			m_output.open(record_path, std::ios::binary | std::ios::trunc);
			if (!m_output)
				fatalerror("Unable to open MVEC record file '%s'\n", record_path);
			write_header();
			m_writer = std::thread(&stream_state::writer_loop, this);
			osd_printf_info("MVEC: recording vector stream to %s\n", record_path);
		}
		else
		{
			m_mode = mode::PLAYBACK;
			m_input.open(playback_path, std::ios::binary);
			if (!m_input)
				fatalerror("Unable to open MVEC playback file '%s'\n", playback_path);
			read_header();
			osd_printf_info("MVEC: playing vector stream from %s\n", playback_path);
		}
	}

	~stream_state()
	{
		finish();
		if (m_claimed)
		{
			std::lock_guard<std::mutex> lock(s_mvec_claim_mutex);
			s_mvec_claimed = false;
		}
	}

	mode current_mode() const { return m_mode; }
	bool recording() const { return m_mode == mode::RECORD; }
	bool playing() const { return m_mode == mode::PLAYBACK; }

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
		if (m_eof)
		{
			copy_cached(dest, capacity, count, true);
			stale = true;
			return true;
		}

		u32 magic;
		if (!read_file_u32(magic, true))
		{
			playback_end("End of file");
			copy_cached(dest, capacity, count, true);
			stale = true;
			return true;
		}
		if (magic != MVEC_FRAME_MAGIC)
			fatalerror("MVEC frame marker is invalid at frame %llu\n", (unsigned long long)m_frames);
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
			fatalerror("MVEC CRC mismatch at frame %llu\n", (unsigned long long)m_frames);

		payload_reader reader(payload);
		const u64 frame_number = reader.get_u64();
		if (frame_number != m_frames)
			fatalerror("MVEC frame sequence mismatch (expected %llu, got %llu)\n", (unsigned long long)m_frames, (unsigned long long)frame_number);
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
		copy_cached(dest, capacity, count, stale);
		++m_frames;
		return true;
	}

private:
	void write_header()
	{
		std::vector<u8> header;
		const char magic[8] = { 'M','A','M','E','V','E','C','\0' };
		header.insert(header.end(), magic, magic + sizeof(magic));
		append_u16(header, MVEC_VERSION_MAJOR); append_u16(header, MVEC_VERSION_MINOR);
		append_u32(header, 0x12345678U);
		append_string(header, m_owner.machine().system().name);
		append_string(header, m_owner.tag());
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
		if (major != MVEC_VERSION_MAJOR || endian != 0x12345678U)
			fatalerror("MVEC format version or byte order is unsupported\n");
		const std::string system = read_file_string();
		const std::string device = read_file_string();
		if (system != m_owner.machine().system().name)
			fatalerror("MVEC file is for machine '%s', not '%s'\n", system.c_str(), m_owner.machine().system().name);
		if (device != m_owner.tag())
			osd_printf_warning("MVEC: recorded device is '%s', current device is '%s'\n", device.c_str(), m_owner.tag());
		osd_printf_info("MVEC format %u.%u, machine %s, device %s\n", major, minor, system.c_str(), device.c_str());
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
	std::string read_file_string()
	{
		u16 size; if (!read_file_u16(size)) fatalerror("MVEC header string is truncated\n");
		std::string result(size, '\0');
		if (size && !read_exact(result.data(), size)) fatalerror("MVEC header string is truncated\n");
		return result;
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
		osd_printf_info("MVEC: playback ended after %llu frames (%s)\n", (unsigned long long)m_frames, reason);
		m_owner.machine().popmessage("Vector playback ended\n%s", reason);
		if (m_owner.machine().options().vector_exit_after_playback())
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
};

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
	stats.timed = frame_timed;
	stats.total_energy = stats_total_energy;
	static_assert(sizeof(stats.offscreen_energy) == sizeof(stats_offscreen_energy));
	memcpy(stats.offscreen_energy, stats_offscreen_energy, sizeof(stats.offscreen_energy));
	static_assert(sizeof(stats.edge_energy) == sizeof(stats_edge_energy));
	memcpy(stats.edge_energy, stats_edge_energy, sizeof(stats.edge_energy));
	screen.container().set_vector_stats(stats);

	return 0;
}
