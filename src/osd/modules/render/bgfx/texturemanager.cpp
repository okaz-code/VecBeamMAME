// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  texturemanager.cpp - BGFX texture manager
//
//  Maintains a string-to-entry mapping for any registered
//  textures.
//
//============================================================

#include "texturemanager.h"

#include <bx/timer.h>

#include "bgfxutil.h"
#include "texture.h"

#include "modules/render/copyutil.h"
#include "osdcore.h"

#include "emucore.h"
#include "fileio.h"
#include "rendutil.h"


texture_manager::texture_manager()
{
	// out-of-line so header works with forward declarations
}

texture_manager::~texture_manager()
{
	m_textures.clear();

	for (std::pair<uint64_t, sequenced_handle> mame_texture : m_mame_textures)
	{
		bgfx::destroy(mame_texture.second.handle);
	}
	m_mame_textures.clear();

	if (bgfx::isValid(m_dummy))
	{
		bgfx::destroy(m_dummy);
		m_dummy = BGFX_INVALID_HANDLE;
	}
}

bgfx::TextureHandle texture_manager::dummy_handle()
{
	if (!bgfx::isValid(m_dummy))
	{
		// 1x1 opaque black, immutable. Sampler-clamped so any UV reads the single texel.
		const uint32_t black = 0xff000000;
		const bgfx::Memory *mem = bgfx::copy(&black, sizeof(black));
		m_dummy = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::BGRA8,
			BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
	}
	return m_dummy;
}

void texture_manager::add_provider(const std::string &name, std::unique_ptr<bgfx_texture_handle_provider> &&provider)
{
	const auto iter = m_textures.find(name);
	if (iter != m_textures.end())
		iter->second = std::make_pair(provider.get(), std::move(provider));
	else
		m_textures.emplace(name, std::make_pair(provider.get(), std::move(provider)));
}

void texture_manager::add_provider(const std::string &name, bgfx_texture_handle_provider &provider)
{
	const auto iter = m_textures.find(name);
	if (iter != m_textures.end())
		iter->second = std::make_pair(&provider, nullptr);
	else
		m_textures.emplace(name, std::make_pair(&provider, nullptr));
}

bgfx_texture* texture_manager::create_texture(
		const std::string &name,
		bgfx::TextureFormat::Enum format,
		uint32_t width,
		uint32_t width_margin,
		uint32_t height,
		void* data,
		uint32_t flags)
{
	auto texture = std::make_unique<bgfx_texture>(name, format, width, width_margin, height, flags, data);
	bgfx_texture &result = *texture;
	m_textures[name] = std::make_pair(texture.get(), std::move(texture));
	return &result;
}

bgfx_texture* texture_manager::create_png_texture(
		const std::string &path,
		const std::string &file_name,
		std::string texture_name,
		uint32_t flags,
		uint32_t screen)
{
	bitmap_argb32 bitmap;
	emu_file file(path, OPEN_FLAG_READ);
	if (!file.open(file_name))
	{
		render_load_png(bitmap, file);
		file.close();
	}

	if (bitmap.width() == 0 || bitmap.height() == 0)
	{
		osd_printf_error("Unable to load PNG '%s' from path '%s'\n", file_name, path);
		return nullptr;
	}

	const uint32_t width = bitmap.width();
	const uint32_t height = bitmap.height();
	auto data32 = std::make_unique<uint32_t []>(width * height);

	const uint32_t rowpixels = bitmap.rowpixels();
	auto* base = reinterpret_cast<uint32_t *>(bitmap.raw_pixptr(0));
	for (int y = 0; y < height; y++)
	{
		copy_util::copyline_argb32_to_bgra(&data32[y * width], base + y * rowpixels, width, nullptr);
	}

	if (screen >= 0)
	{
		texture_name += std::to_string(screen);
	}
	return create_texture(texture_name, bgfx::TextureFormat::BGRA8, width, 0, height, data32.get(), flags);
}


// MAME texture upload accounting.
//
// A cache hit costs nothing, but a miss re-pushes the whole bitmap - and layout artwork is large:
// the Vectrex overlay plate is 1613x2060, 13.3 MB as BGRA, and a view can carry three of them.
// Nothing uploads at all once a static layout has settled, so any steady traffic here means
// something is bumping a texture's seqid every frame, and a burst means it just did. That is worth
// being able to see, because the symptom at the other end is a frame the artwork misses - which
// reads as the bezel flickering, with nothing in the renderer to point at.
//
// The window is closed from the renderer's frame loop rather than from the next upload, or a burst
// followed by silence would be reported whenever the next one happened to arrive - which is to say
// attributed to the wrong second, or not at all.
namespace {

uint32_t s_upload_count = 0;
uint64_t s_upload_pixels = 0;
uint32_t s_upload_largest = 0;

void note_mame_texture_upload(int width, int height)
{
	s_upload_count++;
	s_upload_pixels += uint64_t(width) * uint64_t(height);
	s_upload_largest = std::max(s_upload_largest, uint32_t(width) * uint32_t(height));
}

} // anonymous namespace

// Called once per frame. Reports only seconds that carried traffic, so a settled layout is silent.
void texture_manager::tick_upload_report()
{
	const int64_t now = bx::getHPCounter();
	if (!m_upload_window_begin)
	{
		m_upload_window_begin = now;
		return;
	}
	if ((now - m_upload_window_begin) < bx::getHPFrequency())
		return;

	if (s_upload_count)
	{
		osd_printf_verbose("BGFX: MAME texture uploads %u/s, %.1f MB/s, largest %.1f MB\n",
				s_upload_count, double(s_upload_pixels) * 4.0 / 1.0e6,
				double(s_upload_largest) * 4.0 / 1.0e6);
	}
	s_upload_count = 0;
	s_upload_pixels = 0;
	s_upload_largest = 0;
	m_upload_window_begin = now;
}

bgfx::TextureHandle texture_manager::create_or_update_mame_texture(
		uint32_t format,
		int width,
		int width_margin,
		int height,
		int rowpixels,
		const rgb_t *palette,
		void *base,
		uint32_t seqid,
		uint32_t flags,
		uint64_t key,
		uint64_t old_key)
{
	bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
	if (old_key != ~0ULL)
	{
		const auto iter = m_mame_textures.find(old_key);
		if (iter != m_mame_textures.end())
		{
			handle = iter->second.handle;
			if (handle.idx == bgfx::kInvalidHandle)
				return handle;

			if (iter->second.width == width && iter->second.height == height)
			{
				// Size matches, so let's just swap the old handle into the new location
				m_mame_textures[key] = { handle, seqid, width, height };
				m_mame_textures[old_key] = { BGFX_INVALID_HANDLE, 0, 0, 0 };

				if (iter->second.seqid == seqid)
				{
					// Everything matches, just return the existing handle
					return handle;
				}
				else
				{
					bgfx::TextureFormat::Enum dst_format = bgfx::TextureFormat::BGRA8;
					uint16_t pitch = width;
					int width_div_factor = 1;
					int width_mul_factor = 1;
					const bgfx::Memory* mem = bgfx_util::mame_texture_data_to_bgfx_texture_data(dst_format, format, rowpixels, width_margin, height, palette, base, pitch, width_div_factor, width_mul_factor);
					note_mame_texture_upload(int(width), int(height));
					bgfx::updateTexture2D(handle, 0, 0, 0, 0, uint16_t((rowpixels * width_mul_factor) / width_div_factor), uint16_t(height), mem, pitch);
					return handle;
				}
			}
			bgfx::destroy(handle);
			m_mame_textures[old_key] = { BGFX_INVALID_HANDLE, 0, 0, 0 };
		}
	}
	else
	{
		auto iter = m_mame_textures.find(key);
		if (iter != m_mame_textures.end())
		{
			handle = iter->second.handle;
			if (handle.idx == bgfx::kInvalidHandle)
				return handle;

			if (iter->second.width == width && iter->second.height == height)
			{
				if (iter->second.seqid == seqid)
				{
					return handle;
				}
				else
				{
					bgfx::TextureFormat::Enum dst_format = bgfx::TextureFormat::BGRA8;
					uint16_t pitch = width;
					int width_div_factor = 1;
					int width_mul_factor = 1;
					const bgfx::Memory* mem = bgfx_util::mame_texture_data_to_bgfx_texture_data(dst_format, format, rowpixels, width_margin, height, palette, base, pitch, width_div_factor, width_mul_factor);
					note_mame_texture_upload(int(width), int(height));
					bgfx::updateTexture2D(handle, 0, 0, 0, 0, uint16_t((rowpixels * width_mul_factor) / width_div_factor), uint16_t(height), mem, pitch);
					return handle;
				}
			}
			bgfx::destroy(handle);
		}
	}

	bgfx::TextureFormat::Enum dst_format = bgfx::TextureFormat::BGRA8;
	uint16_t pitch = width;
	int width_div_factor = 1;
	int width_mul_factor = 1;
	const bgfx::Memory* mem = bgfx_util::mame_texture_data_to_bgfx_texture_data(dst_format, format, rowpixels, width_margin, height, palette, base, pitch, width_div_factor, width_mul_factor);
	const uint16_t adjusted_width = uint16_t((rowpixels * width_mul_factor) / width_div_factor);
	handle = bgfx::createTexture2D(adjusted_width, height, false, 1, dst_format, flags, nullptr);
	note_mame_texture_upload(int(width), int(height));
					bgfx::updateTexture2D(handle, 0, 0, 0, 0, adjusted_width, uint16_t(height), mem, pitch);

	m_mame_textures[key] = { handle, seqid, width, height };
	return handle;
}

bgfx::TextureHandle texture_manager::handle(const std::string &name)
{
	bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
	const auto iter = m_textures.find(name);
	if (iter != m_textures.end())
		handle = iter->second.first->texture();

	assert(handle.idx != bgfx::kInvalidHandle);
	return handle;
}

bgfx_texture_handle_provider* texture_manager::provider(const std::string &name)
{
	const auto iter = m_textures.find(name);
	if (iter != m_textures.end())
		return iter->second.first;
	else
		return nullptr;
}

void texture_manager::remove_provider(const std::string &name)
{
	m_textures.erase(name);
}
