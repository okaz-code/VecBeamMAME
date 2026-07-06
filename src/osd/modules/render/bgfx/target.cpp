// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  target.cpp - Render target abstraction for BGFX layer
//
//============================================================

#include "emucore.h"

#include "target.h"

#include <cstring>

bgfx_target::bgfx_target(std::string name, bgfx::TextureFormat::Enum format, uint16_t width, uint16_t height, uint16_t xprescale, uint16_t yprescale,
	uint32_t style, bool double_buffer, bool filter, float scale, uint32_t screen)
	: m_name(name)
	, m_format(format)
	, m_targets(nullptr)
	, m_textures(nullptr)
	, m_width(width)
	, m_height(height)
	, m_xprescale(xprescale)
	, m_yprescale(yprescale)
	, m_double_buffer(double_buffer)
	, m_style(style)
	, m_filter(filter)
	, m_scale(scale)
	, m_screen(screen)
	, m_current_page(0)
	, m_initialized(false)
	, m_page_count(double_buffer ? 2 : 1)
{
	if (m_width > 0 && m_height > 0)
	{
		// fractional scales are allowed (e.g. 0.5 for half-resolution bloom targets);
		// round to nearest and keep at least 1px
		const float scaled_w = float(m_width) * m_scale;
		const float scaled_h = float(m_height) * m_scale;
		m_width  = (scaled_w < 1.0f) ? uint16_t(1) : uint16_t(scaled_w + 0.5f);
		m_height = (scaled_h < 1.0f) ? uint16_t(1) : uint16_t(scaled_h + 0.5f);

		uint32_t wrap_mode = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
		uint32_t filter_mode = filter ? (BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC) : (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT);
		uint32_t depth_flags = wrap_mode | (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT);

		m_textures = new bgfx::TextureHandle[m_page_count * 2];
		m_targets = new bgfx::FrameBufferHandle[m_page_count];
		// Zero-initialize the colour texture's backing memory explicitly: bgfx::createTexture2D with no
		// _mem leaves the GPU texture's content genuinely undefined (not guaranteed zero by any backend
		// or driver). For most targets a full-screen pass overwrites every pixel before it's ever read,
		// so this never matters - but a target that's READ by a self-referential feedback pass before
		// anything has ever WRITTEN it (e.g. the vector phosphor pool's "previous frame" sampler, read
		// on the very first frame after this target is freshly (re)created by a chain switch/reload/
		// resolution change) would otherwise see arbitrary garbage, which that pass's own logic could
		// misinterpret as legitimate held content for a long time (phosphor_total_ms) before naturally
		// clearing. All-zero bytes are exactly 0.0 in every bgfx texture format used here (integer
		// normalized, half-float, and float all represent 0 as an all-zero bit pattern), so a single
		// generic zero-fill is correct regardless of which of this class's callers/formats is in use.
		bgfx::TextureInfo tex_info;
		bgfx::calcTextureSize(tex_info, m_width * xprescale, m_height * yprescale, 1, false, false, 1, format);
		for (int page = 0; page < m_page_count; page++)
		{
			const bgfx::Memory *zero_mem = bgfx::alloc(tex_info.storageSize);
			std::memset(zero_mem->data, 0, zero_mem->size);
			m_textures[page] = bgfx::createTexture2D(m_width * xprescale, m_height * yprescale, false, 1, format, wrap_mode | filter_mode | BGFX_TEXTURE_RT, zero_mem);
			assert(m_textures[page].idx != 0xffff);

			m_textures[m_page_count + page] = bgfx::createTexture2D(m_width * xprescale, m_height * yprescale, false, 1, bgfx::TextureFormat::D32F, depth_flags | BGFX_TEXTURE_RT);
			assert(m_textures[m_page_count + page].idx != 0xffff);

			bgfx::TextureHandle handles[2] = { m_textures[page], m_textures[m_page_count + page] };
			m_targets[page] = bgfx::createFrameBuffer(2, handles, false);

			assert(m_targets[page].idx != 0xffff);
		}

		m_initialized = true;
	}
}

bgfx_target::bgfx_target(void *handle, uint16_t width, uint16_t height)
	: m_name("backbuffer")
	, m_format(bgfx::TextureFormat::Unknown)
	, m_targets(nullptr)
	, m_textures(nullptr)
	, m_width(width)
	, m_height(height)
	, m_xprescale(1)
	, m_yprescale(1)
	, m_double_buffer(false)
	, m_style(TARGET_STYLE_CUSTOM)
	, m_filter(false)
	, m_scale(0)
	, m_screen(-1)
	, m_current_page(0)
	, m_initialized(true)
	, m_page_count(0)
{
	m_targets = new bgfx::FrameBufferHandle[1];
	m_targets[0] = bgfx::createFrameBuffer(handle, width, height, bgfx::TextureFormat::Count, bgfx::TextureFormat::D32F);

	// No backing texture
}

bgfx_target::~bgfx_target()
{
	if (!m_initialized)
	{
		return;
	}

	if (m_page_count > 0)
	{
		for (int page = 0; page < m_page_count; page++)
		{
			bgfx::destroy(m_targets[page]);
			bgfx::destroy(m_textures[m_page_count + page]);
			bgfx::destroy(m_textures[page]);
		}
		delete [] m_textures;
		delete [] m_targets;
	}
	else
	{
		bgfx::destroy(m_targets[0]);
		delete [] m_targets;
	}
}

void bgfx_target::page_flip()
{
	if (!m_initialized) return;

	if (m_double_buffer)
	{
		m_current_page = 1 - m_current_page;
	}
}

bgfx::FrameBufferHandle bgfx_target::target()
{
	if (!m_initialized) return BGFX_INVALID_HANDLE;
	return m_targets[m_current_page];
}

bgfx::TextureHandle bgfx_target::texture() const
{
	if (!m_initialized) return BGFX_INVALID_HANDLE;

	if (m_double_buffer)
	{
		return m_textures[1 - m_current_page];
	}
	else
	{
		return m_textures[m_current_page];
	}
}
