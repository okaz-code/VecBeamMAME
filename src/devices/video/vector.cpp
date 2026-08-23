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

#include "util/hashing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>


#define VECTOR_WIDTH_DENOM 512

// 20000 is needed for mhavoc (see MT 06668) 10000 is enough for other games
#define MAX_POINTS 20000

float vector_options::s_flicker = 0.0f;
float vector_options::s_beam_width_min = 0.0f;
float vector_options::s_beam_width_max = 0.0f;
float vector_options::s_beam_dot_size = 0.0f;
float vector_options::s_beam_intensity_weight = 0.0f;

void vector_options::init(emu_options &options)
{
	s_beam_width_min = options.beam_width_min();
	s_beam_width_max = options.beam_width_max();
	s_beam_dot_size = options.beam_dot_size();
	s_beam_intensity_weight = options.beam_intensity_weight();
	s_flicker = options.flicker();
}

/*
 * MVEC playback (-vector_playback)
 *
 * Reads a beam-event stream recorded by VecBeamMAME and substitutes it for the emulated beam
 * list, so the same beam events can be rendered by this build for comparison. The format is
 * documented in mame_doc/mvec-format-v1.md; this reader implements v1.0 and v1.1.
 *
 * Recording is deliberately not implemented, and every field the stock point struct has no
 * home for - beam energy, the per-point sweep timestamps, the line-cap terminus bits and the
 * Vectrex source metadata - is parsed and discarded. The record layout is fixed-size and
 * tag-free, so it has to be walked in full even to skip it.
 */

namespace {

constexpr u32 MVEC_FRAME_MAGIC = 0x4d415246U; // "FRAM" in little-endian byte order
constexpr u16 MVEC_VERSION_MAJOR = 1;
constexpr u16 MVEC_VERSION_MINOR = 1;
constexpr u32 MVEC_POINT_SIZE = 66U;
constexpr u32 MVEC_PAYLOAD_HEADER_SIZE = 36U;
constexpr u64 MVEC_INVALID_POSITION = ~u64(0);

// Reads little-endian fields out of one frame payload. The format has no field tags, so a
// field is skipped by reading it and throwing the value away, never by seeking past it.
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
private:
	void require(size_t count) { if (count > m_data.size() - m_pos) fatalerror("MVEC frame is truncated\n"); }
	const std::vector<u8> &m_data;
	size_t m_pos = 0;
};

} // anonymous namespace


class vector_device::stream_state
{
public:
	stream_state(vector_device &owner) : m_owner(owner)
	{
		const char *const path = owner.machine().options().vector_playback();
		if (!path || !path[0])
			return;

		m_input.open(path, std::ios::binary);
		if (!m_input)
		{
			osd_printf_error("MVEC: playback file '%s' was not found or could not be opened; playback is disabled\n", path);
			return;
		}
		read_header();
		m_playing = true;
		osd_printf_info("MVEC: playing vector stream from %s\n", path);
		announce("Loaded");
	}

	bool playing() const { return m_playing; }

	// Fills dest with the points of the frame the tool state selects, and reports the visible
	// area they were recorded against. Both are needed: this build's driver may declare a
	// different visible area than the one the stream was recorded with, and the geometry has to
	// be normalised against the recorded one or the image is skewed.
	void playback_frame(point *dest, int capacity, int &count, rectangle &visarea)
	{
		poll_tool();

		u64 target;
		if (m_pending != MVEC_INVALID_POSITION)
		{
			target = m_pending;
			m_pending = MVEC_INVALID_POSITION;
		}
		else if (m_position == MVEC_INVALID_POSITION)
		{
			target = 0;
		}
		else
		{
			target = m_paused ? m_position : m_position + 1U;
		}

		if (target >= m_index.size())
		{
			// Hold the last frame at end of file rather than going black.
			if (!m_at_end)
			{
				m_at_end = true;
				announce("End of file");
			}
			emit(dest, capacity, count, visarea);
			return;
		}

		if (target != m_position)
		{
			load(target);
			m_position = target;
			m_at_end = false;
		}
		emit(dest, capacity, count, visarea);
	}

	// The stream's own frame rate. Playback advances one MVEC frame per screen update, so this
	// only matches real time if the driver runs at the recorded rate; the constructor warns when
	// it does not.
	attotime frame_period() const
	{
		return (m_frame_period > 0) ? attotime(0, m_frame_period) : m_owner.screen().frame_period();
	}

private:
	struct index_entry
	{
		std::streamoff offset = 0;
		u64 content_frame = MVEC_INVALID_POSITION;
		u32 point_count = 0;
		rectangle visarea;
		bool stale = false;
	};

	//----------------------------------------------------------------------------------------
	//  file reading
	//----------------------------------------------------------------------------------------

	bool read_exact(void *dest, size_t length)
	{
		m_input.read(reinterpret_cast<char *>(dest), std::streamsize(length));
		return bool(m_input) && (size_t(m_input.gcount()) == length);
	}

	bool read_file_u16(u16 &value)
	{
		u8 b[2];
		if (!read_exact(b, sizeof(b))) return false;
		value = u16(b[0]) | (u16(b[1]) << 8);
		return true;
	}

	bool read_file_u32(u32 &value)
	{
		u8 b[4];
		if (!read_exact(b, sizeof(b))) return false;
		value = u32(b[0]) | (u32(b[1]) << 8) | (u32(b[2]) << 16) | (u32(b[3]) << 24);
		return true;
	}

	bool read_file_i64(s64 &value)
	{
		u8 b[8];
		if (!read_exact(b, sizeof(b))) return false;
		u64 v = 0;
		for (unsigned i = 0; i < 8; ++i) v |= u64(b[i]) << (i * 8);
		value = s64(v);
		return true;
	}

	std::string read_file_string()
	{
		u16 length;
		if (!read_file_u16(length))
			fatalerror("MVEC header string is truncated\n");
		std::string out(length, '\0');
		if (length && !read_exact(out.data(), length))
			fatalerror("MVEC header string is truncated\n");
		return out;
	}

	void read_header()
	{
		char magic[8];
		if (!read_exact(magic, sizeof(magic)) || std::memcmp(magic, "MAMEVEC\0", 8))
			fatalerror("MVEC file is corrupt or unsupported\n");
		u16 major, minor;
		u32 endian;
		if (!read_file_u16(major) || !read_file_u16(minor) || !read_file_u32(endian))
			fatalerror("MVEC header is truncated\n");
		if (major != MVEC_VERSION_MAJOR || minor > MVEC_VERSION_MINOR || endian != 0x12345678U)
			fatalerror("MVEC format version or byte order is unsupported\n");
		const std::string system = read_file_string();
		const std::string device = read_file_string();
		// Version 1.0 predates the recorded frame period.
		if (minor >= 1 && !read_file_i64(m_frame_period))
			fatalerror("MVEC recorded frame period is truncated\n");
		if (system != m_owner.machine().system().name)
			fatalerror("MVEC file is for machine '%s', not '%s'\n", system, m_owner.machine().system().name);
		if (device != m_owner.tag())
			osd_printf_warning("MVEC: recorded device is '%s', current device is '%s'\n", device, m_owner.tag());
		osd_printf_info("MVEC format %u.%u, machine %s, device %s\n", major, minor, system, device);

		build_index();
		if (m_frame_period <= 0)
			m_frame_period = infer_frame_period();
		report_rate();
	}

	void build_index()
	{
		u64 last_content = MVEC_INVALID_POSITION;
		for (u64 expected = 0; ; ++expected)
		{
			const std::streamoff offset = m_input.tellg();
			u32 magic;
			if (!read_file_u32(magic))
				break;
			if (magic != MVEC_FRAME_MAGIC)
				fatalerror("MVEC frame marker is invalid at frame %d\n", int(expected));
			u32 payload_size;
			if (!read_file_u32(payload_size) || payload_size > 128U * 1024U * 1024U)
				fatalerror("MVEC frame size is invalid\n");
			std::vector<u8> payload(payload_size);
			if (!read_exact(payload.data(), payload.size()))
				fatalerror("MVEC frame payload is truncated\n");
			u32 stored_crc;
			if (!read_file_u32(stored_crc))
				fatalerror("MVEC frame checksum is missing\n");
			if (stored_crc != u32(util::crc32_creator::simple(payload.data(), u32(payload.size()))))
				fatalerror("MVEC CRC mismatch at frame %d\n", int(expected));

			payload_reader reader(payload);
			if (reader.get_u64() != expected)
				fatalerror("MVEC frame sequence mismatch while indexing\n");
			const u32 flags = reader.get_u32();
			index_entry entry;
			entry.offset = offset;
			entry.stale = bool(flags & 1U);
			reader.get_u32(); // generation: recorded, not used here
			entry.visarea.min_x = reader.get_i32(); entry.visarea.max_x = reader.get_i32();
			entry.visarea.min_y = reader.get_i32(); entry.visarea.max_y = reader.get_i32();
			entry.point_count = reader.get_u32();
			if (entry.point_count > MAX_POINTS
					|| payload_size != MVEC_PAYLOAD_HEADER_SIZE + entry.point_count * MVEC_POINT_SIZE)
				fatalerror("MVEC frame payload size is invalid at frame %d\n", int(expected));
			if (entry.stale)
			{
				if (entry.point_count)
					fatalerror("MVEC stale frame unexpectedly contains points\n");
				entry.content_frame = last_content;
			}
			else
			{
				entry.content_frame = expected;
				last_content = expected;
			}
			m_index.emplace_back(entry);
		}
		m_input.clear();
		if (m_index.empty())
			fatalerror("MVEC file contains no frames\n");
		osd_printf_info("MVEC: indexed %d frames\n", int(m_index.size()));
	}

	// A stale frame carries no points and re-displays the last content frame, so seeking to one
	// means decoding that content frame instead, while the stale frame still supplies its own
	// visible area.
	void load(u64 target)
	{
		const index_entry &entry = m_index[target];
		if (entry.stale)
		{
			if (entry.content_frame != MVEC_INVALID_POSITION && m_cached_frame != entry.content_frame)
				decode(entry.content_frame);
			else if (entry.content_frame == MVEC_INVALID_POSITION)
				m_cached.clear();
			m_visarea = entry.visarea;
		}
		else
		{
			decode(target);
		}
	}

	void decode(u64 index)
	{
		const index_entry &entry = m_index[index];
		m_input.clear();
		m_input.seekg(entry.offset);
		if (!m_input)
			fatalerror("MVEC seek failed at frame %d\n", int(index));

		u32 magic, payload_size, stored_crc;
		if (!read_file_u32(magic) || magic != MVEC_FRAME_MAGIC)
			fatalerror("MVEC frame marker is invalid at frame %d\n", int(index));
		if (!read_file_u32(payload_size) || payload_size > 128U * 1024U * 1024U)
			fatalerror("MVEC frame size is invalid\n");
		std::vector<u8> payload(payload_size);
		if (!read_exact(payload.data(), payload.size()))
			fatalerror("MVEC frame payload is truncated\n");
		if (!read_file_u32(stored_crc))
			fatalerror("MVEC frame checksum is missing\n");
		if (stored_crc != u32(util::crc32_creator::simple(payload.data(), u32(payload.size()))))
			fatalerror("MVEC CRC mismatch at frame %d\n", int(index));

		payload_reader reader(payload);
		if (reader.get_u64() != index)
			fatalerror("MVEC frame sequence mismatch at frame %d\n", int(index));
		reader.get_u32(); // flags: already known from the index
		reader.get_u32(); // generation
		m_visarea.min_x = reader.get_i32(); m_visarea.max_x = reader.get_i32();
		m_visarea.min_y = reader.get_i32(); m_visarea.max_y = reader.get_i32();
		const u32 count = reader.get_u32();

		m_cached.resize(count);
		int previous_x = 0, previous_y = 0;
		for (u32 i = 0; i < count; ++i)
		{
			point &p = m_cached[i];
			// x0/y0 is the segment start. This build draws from the previous point, exactly as
			// the recorder's own lists do - every point of every capture examined has
			// (x0, y0) equal to the previous point's (x, y) - so it is only checked here.
			const s32 x0 = reader.get_i32();
			const s32 y0 = reader.get_i32();
			p.x = reader.get_i32();
			p.y = reader.get_i32();
			p.col = rgb_t(reader.get_u32());
			p.intensity = reader.get_u8();
			// Everything below has no home in this build's point struct. The record is a
			// fixed-size layout with no field tags, so it is read and dropped.
			reader.get_float();                      // beam_energy
			reader.get_i32(); reader.get_i64();      // t0
			reader.get_i32(); reader.get_i64();      // t1
			reader.get_u32();                        // cap_flags
			reader.get_i32();                        // dump_scale
			reader.get_double();                     // dump_ramp_us
			reader.get_u8();                         // dump_midchange
			if (i && (x0 != previous_x || y0 != previous_y) && !m_warned_discontinuous)
			{
				m_warned_discontinuous = true;
				osd_printf_warning("MVEC: frame %d point %d starts at (%d,%d) but the previous point ended at (%d,%d);"
						" this build draws from the previous point, so the segment will differ\n",
						int(index), int(i), x0, y0, previous_x, previous_y);
			}
			previous_x = p.x;
			previous_y = p.y;
		}
		m_cached_frame = index;
	}

	void emit(point *dest, int capacity, int &count, rectangle &visarea)
	{
		count = int(std::min<size_t>(m_cached.size(), size_t(capacity)));
		std::copy_n(m_cached.begin(), count, dest);
		visarea = m_visarea;
	}

	//----------------------------------------------------------------------------------------
	//  rate check
	//----------------------------------------------------------------------------------------

	// Format 1.0 stored no frame period, and every 1.0 capture in hand needs one to be able to
	// say whether this driver's rate matches. A timed stream still carries an absolute t0 per
	// point, so the period can be recovered from how far the first point's timestamp advances
	// across the stream. Only that one timestamp is read out of a bounded sample of frames - the
	// payloads run to gigabytes, so seek to each one rather than walking them.
	//
	// The estimator is the total span divided by the number of frames it covers, NOT the median
	// per-frame advance. A list-start interval is not a fixed quantity: on starwars.mvec the
	// advances run from 12.2 ms at the 5th percentile to 24.5 ms at the 95th, because the AVG is
	// restarted by the game and a heavy frame takes longer. That distribution is skewed right, so
	// its median reads 16.26 ms (61.5 Hz) where the true average is 16.67 ms (60.0 Hz). Playback
	// consumes one recorded frame per screen update, so the average is the quantity that matters.
	attoseconds_t infer_frame_period()
	{
		const size_t stride = std::max<size_t>(1, m_index.size() / 4096);
		double first_time = 0.0, last_time = 0.0;
		size_t first_frame = 0, last_frame = 0;
		unsigned samples = 0;
		for (size_t i = 0; i < m_index.size(); i += stride)
		{
			const index_entry &entry = m_index[i];
			if (entry.stale || !entry.point_count)
				continue;

			// frame marker and size (8) + payload header (36) + the point fields ahead of t0 (25)
			m_input.clear();
			m_input.seekg(entry.offset + std::streamoff(69));
			u8 bytes[12];
			if (!read_exact(bytes, sizeof(bytes)))
				continue;
			s32 seconds = 0;
			for (unsigned b = 0; b < 4; ++b) seconds |= s32(u32(bytes[b]) << (b * 8));
			u64 attoseconds = 0;
			for (unsigned b = 0; b < 8; ++b) attoseconds |= u64(bytes[4 + b]) << (b * 8);
			// An untimed point carries attotime::never, which is not a clock reading.
			if (seconds < 0 || seconds >= ATTOTIME_MAX_SECONDS)
				continue;

			const double now = double(seconds) + double(s64(attoseconds)) / double(ATTOSECONDS_PER_SECOND);
			if (!samples)
			{
				first_time = now;
				first_frame = i;
			}
			last_time = now;
			last_frame = i;
			++samples;
		}
		if (samples < 2 || last_frame <= first_frame)
			return 0;

		const double mean = (last_time - first_time) / double(last_frame - first_frame);
		if (!(mean > 0.0) || mean > 1.0)
			return 0;
		const attoseconds_t period = attoseconds_t(mean * double(ATTOSECONDS_PER_SECOND));
		osd_printf_info("MVEC: no recorded frame rate; estimated %f Hz from the beam timestamps across frames %d to %d\n",
				attotime(0, period).as_hz(), int(first_frame), int(last_frame));
		return period;
	}

	// Playback advances one recorded frame per screen update, so a driver running at a different
	// rate than the recording plays it back at the wrong speed. This is easy to miss by eye over
	// a short clip and fatal to a side-by-side comparison, so say so loudly.
	void report_rate()
	{
		if (m_frame_period <= 0)
		{
			osd_printf_warning("MVEC: the stream records no frame rate and none could be estimated from it;"
					" assuming this screen's %f Hz. If it was recorded at another rate, playback runs at"
					" the wrong speed\n",
					m_owner.screen().frame_period().as_hz());
			return;
		}
		const double recorded = attotime(0, m_frame_period).as_hz();
		const double current = m_owner.screen().frame_period().as_hz();
		osd_printf_info("MVEC: playback source rate %f Hz\n", recorded);
		if (std::fabs(recorded - current) > 0.5)
		{
			osd_printf_warning("MVEC: stream was recorded at %f Hz but this screen runs at %f Hz."
					" Playback advances one recorded frame per screen update, so it will run %s"
					" than the recording\n",
					recorded, current, (current < recorded) ? "slower" : "faster");
		}
	}

	//----------------------------------------------------------------------------------------
	//  playback controls
	//----------------------------------------------------------------------------------------

	void request_position(u64 position)
	{
		m_pending = std::min<u64>(position, m_index.empty() ? 0U : m_index.size() - 1U);
		m_paused = true;
		announce("Go to");
	}

	void request_relative(s64 delta)
	{
		const s64 base = (m_position == MVEC_INVALID_POSITION) ? 0 : s64(m_position);
		m_pending = u64(std::max<s64>(0, base + delta));
		m_paused = true;
		announce(delta < 0 ? "Back" : "Forward");
	}

	// Same chords as the recorder's own playback tool, so the two builds can be driven
	// side by side without relearning them. Everything sits behind Alt (Option on macOS) to
	// stay clear of the game's own inputs.
	void poll_tool()
	{
		input_manager &input = m_owner.machine().input();

		if (m_goto_mode)
		{
			static const input_code digits[10] = {
				KEYCODE_0, KEYCODE_1, KEYCODE_2, KEYCODE_3, KEYCODE_4,
				KEYCODE_5, KEYCODE_6, KEYCODE_7, KEYCODE_8, KEYCODE_9 };
			for (int digit = 0; digit < 10; ++digit)
				if (input.code_pressed_once(digits[digit]) && m_goto_digits.size() < 19)
					m_goto_digits += char('0' + digit);
			if (input.code_pressed_once(KEYCODE_BACKSPACE) && !m_goto_digits.empty())
				m_goto_digits.pop_back();
			if (input.code_pressed_once(KEYCODE_ESC))
			{
				m_goto_mode = false;
				m_goto_digits.clear();
				announce("Go to cancelled");
				return;
			}
			if (input.code_pressed_once(KEYCODE_ENTER) || input.code_pressed_once(KEYCODE_ENTER_PAD))
			{
				if (!m_goto_digits.empty())
				{
					u64 value = 0;
					for (char c : m_goto_digits)
						value = std::min<u64>(MVEC_INVALID_POSITION / 10U, value) * 10U + u64(c - '0');
					// The prompt is 1-based, matching what the position readout shows.
					request_position(value ? value - 1U : 0U);
				}
				m_goto_mode = false;
				m_goto_digits.clear();
				return;
			}
			m_owner.machine().popmessage("MVEC  go to frame: %s_", m_goto_digits);
			return;
		}

		if (!input.code_pressed(KEYCODE_LALT) && !input.code_pressed(KEYCODE_RALT))
			return;

		if (input.code_pressed_once(KEYCODE_P))
		{
			m_paused = !m_paused;
			announce(m_paused ? "Paused" : "Playing");
		}
		else if (input.code_pressed_once(KEYCODE_LEFT)) request_relative(-1);
		else if (input.code_pressed_once(KEYCODE_RIGHT)) request_relative(1);
		else if (input.code_pressed_once(KEYCODE_PGUP)) request_relative(-60);
		else if (input.code_pressed_once(KEYCODE_PGDN)) request_relative(60);
		else if (input.code_pressed_once(KEYCODE_HOME)) request_position(0);
		else if (input.code_pressed_once(KEYCODE_END)) request_position(m_index.size() - 1U);
		else if (input.code_pressed_once(KEYCODE_G))
		{
			m_goto_mode = true;
			m_goto_digits.clear();
			m_paused = true;
		}
	}

	// The recorder keeps a persistent overlay through its own UI hook; this build has no such
	// hook and does not add one, so the position is reported on change instead.
	void announce(const char *status)
	{
		const u64 shown = ((m_pending != MVEC_INVALID_POSITION) ? m_pending
				: (m_position == MVEC_INVALID_POSITION ? 0U : m_position)) + 1U;
		m_owner.machine().popmessage("MVEC  %s  frame %d / %d%s",
				status, int(shown), int(m_index.size()), m_paused ? "  [paused]" : "");
	}

	vector_device &m_owner;
	std::ifstream m_input;
	std::vector<index_entry> m_index;
	std::vector<point> m_cached;
	rectangle m_visarea;
	attoseconds_t m_frame_period = 0;
	u64 m_position = MVEC_INVALID_POSITION;
	u64 m_pending = MVEC_INVALID_POSITION;
	u64 m_cached_frame = MVEC_INVALID_POSITION;
	bool m_playing = false;
	bool m_paused = false;
	bool m_at_end = false;
	bool m_goto_mode = false;
	bool m_warned_discontinuous = false;
	std::string m_goto_digits;
};


// device type definition
DEFINE_DEVICE_TYPE(VECTOR, vector_device, "vector_device", "VECTOR")

vector_device::vector_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, VECTOR, tag, owner, clock),
		device_video_interface(mconfig, *this),
		m_vector_list(nullptr),
		m_min_intensity(255),
		m_max_intensity(0)
{
}

vector_device::~vector_device()
{
}

void vector_device::device_start()
{
	vector_options::init(machine().options());

	m_vector_index = 0;

	/* allocate memory for tables */
	m_vector_list = std::make_unique<point[]>(MAX_POINTS);

	m_stream = std::make_unique<stream_state>(*this);
	if (!m_stream->playing())
		m_stream.reset();
}

void vector_device::device_stop()
{
	m_stream.reset();
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

void vector_device::add_point(int x, int y, rgb_t color, int intensity)
{
	point *newpoint;

	intensity = std::clamp(intensity, 0, 255);

	m_min_intensity = intensity > 0 ? std::min(m_min_intensity, intensity) : m_min_intensity;
	m_max_intensity = intensity > 0 ? std::max(m_max_intensity, intensity) : m_max_intensity;

	if (vector_options::s_flicker && (intensity > 0))
	{
		float random = float(machine().rand() & 255) / 255.0f; // random value between 0.0 and 1.0

		intensity -= int(intensity * random * vector_options::s_flicker);

		intensity = std::clamp(intensity, 0, 255);
	}

	newpoint = &m_vector_list[m_vector_index];
	newpoint->x = x;
	newpoint->y = y;
	newpoint->col = color;
	newpoint->intensity = intensity;

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
}

//-------------------------------------------------
// Update the screen container with queued vectors.
//-------------------------------------------------

uint32_t vector_device::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	uint32_t flags = PRIMFLAG_ANTIALIAS(1) | PRIMFLAG_BLENDMODE(BLENDMODE_ADD) | PRIMFLAG_VECTOR(1);
	// Not a reference: MVEC playback substitutes the recorded visible area, which need not be the
	// one this build's driver declares. Normalising against the recorded area is what keeps the
	// replayed geometry identical to the recording.
	rectangle visarea = screen.visible_area();
	if (m_stream)
		m_stream->playback_frame(m_vector_list.get(), MAX_POINTS, m_vector_index, visarea);
	float xscale = 1.0f / (65536 * visarea.width());
	float yscale = 1.0f / (65536 * visarea.height());
	float xoffs = (float)visarea.min_x;
	float yoffs = (float)visarea.min_y;

	point *curpoint;
	int lastx = 0;
	int lasty = 0;

	curpoint = m_vector_list.get();

	screen.container().empty();
	screen.container().add_rect(0.0f, 0.0f, 1.0f, 1.0f, rgb_t(0xff,0x00,0x00,0x00), PRIMFLAG_BLENDMODE(BLENDMODE_ALPHA) | PRIMFLAG_VECTORBUF(1));

	m_frame_begin_notifier();

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
		if (lastx == curpoint->x && lasty == curpoint->y)
			beam_width *= vector_options::s_beam_dot_size;

		coords.x0 = (float(lastx) - xoffs) * xscale;
		coords.y0 = (float(lasty) - yoffs) * yscale;
		coords.x1 = (float(curpoint->x) - xoffs) * xscale;
		coords.y1 = (float(curpoint->y) - yoffs) * yscale;

		if (curpoint->intensity != 0)
		{
			screen.container().add_line(
					coords.x0, coords.y0, coords.x1, coords.y1,
					beam_width,
					(curpoint->intensity << 24) | (curpoint->col & 0xffffff),
					flags);
			m_line_notifier(lastx, lasty, curpoint->x, curpoint->y, curpoint->col, curpoint->intensity, visarea.width(), visarea.height());
		}
		else
		{
			m_move_notifier(curpoint->x, curpoint->y, curpoint->col, visarea.width(), visarea.height());
		}

		lastx = curpoint->x;
		lasty = curpoint->y;

		curpoint++;
	}

	m_frame_end_notifier();

	return 0;
}
