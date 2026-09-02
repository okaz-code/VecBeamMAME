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
#include <vector>

bgfx_target::bgfx_target(std::string name, bgfx::TextureFormat::Enum format, uint16_t width, uint16_t height, uint16_t xprescale, uint16_t yprescale,
	uint32_t style, bool double_buffer, bool filter, float scale, uint32_t screen, uint32_t attachment_count)
	: m_name(name)
	, m_format(format)
	, m_attachments(attachment_count < 1 ? 1 : attachment_count)
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

		// Layout: colour attachment a of page p at [a * page_count + p], depth for page p last.
		// With one attachment this is exactly the previous [page] / [page_count + page] arrangement.
		m_textures = new bgfx::TextureHandle[m_page_count * (m_attachments + 1)];
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
		std::vector<bgfx::TextureHandle> handles(m_attachments + 1);
		for (int page = 0; page < m_page_count; page++)
		{
			for (uint32_t a = 0; a < m_attachments; a++)
			{
				const bgfx::Memory *zero_mem = bgfx::alloc(tex_info.storageSize);
				std::memset(zero_mem->data, 0, zero_mem->size);
				bgfx::TextureHandle &tex = m_textures[a * m_page_count + page];
				tex = bgfx::createTexture2D(m_width * xprescale, m_height * yprescale, false, 1, format, wrap_mode | filter_mode | BGFX_TEXTURE_RT, zero_mem);
				assert(tex.idx != 0xffff);
				handles[a] = tex;
			}

			bgfx::TextureHandle &depth = m_textures[m_attachments * m_page_count + page];
			depth = bgfx::createTexture2D(m_width * xprescale, m_height * yprescale, false, 1, bgfx::TextureFormat::D32F, depth_flags | BGFX_TEXTURE_RT);
			assert(depth.idx != 0xffff);
			handles[m_attachments] = depth;

			m_targets[page] = bgfx::createFrameBuffer(uint8_t(m_attachments + 1), handles.data(), false);
			assert(m_targets[page].idx != 0xffff);
		}

		m_initialized = true;
	}
}

bgfx_target::bgfx_target(void *handle, uint16_t width, uint16_t height)
	: m_name("backbuffer")
	, m_format(bgfx::TextureFormat::Unknown)
	, m_attachments(1)
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
			bgfx::destroy(m_textures[m_attachments * m_page_count + page]);
			for (uint32_t a = 0; a < m_attachments; a++)
				bgfx::destroy(m_textures[a * m_page_count + page]);
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

uint32_t bgfx_target::clear(uint32_t view)
{
	if (!m_initialized || m_page_count == 0 || width() == 0 || height() == 0)
		return 0;

	// Clear both pages of feedback targets and leave the active page unchanged.  This is used for
	// temporal discontinuities (for example an MVEC seek), where recreating the whole chain would
	// leave one frame without the HDR composite and make SDR-white UI flash at HDR peak.
	for (uint32_t page = 0; page < m_page_count; page++)
	{
		const uint16_t clear_view = uint16_t(view + page);
		bgfx::setViewFrameBuffer(clear_view, target());
		bgfx::setViewRect(clear_view, 0, 0, width(), height());
		bgfx::setViewClear(clear_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
		bgfx::setViewMode(clear_view, bgfx::ViewMode::Sequential);
		bgfx::touch(clear_view);
		page_flip();
	}

	return m_page_count;
}

bgfx::FrameBufferHandle bgfx_target::target()
{
	if (!m_initialized) return BGFX_INVALID_HANDLE;
	return m_targets[m_current_page];
}

bgfx::TextureHandle bgfx_target::texture() const
{
	return texture(0);
}

bgfx::TextureHandle bgfx_target::texture(uint32_t attachment) const
{
	if (!m_initialized || m_page_count == 0) return BGFX_INVALID_HANDLE;
	if (attachment >= m_attachments) attachment = 0;
	// A double-buffered target is READ from the page that is not currently being written.
	const uint32_t page = m_double_buffer ? (1 - m_current_page) : m_current_page;
	return m_textures[attachment * m_page_count + page];
}
