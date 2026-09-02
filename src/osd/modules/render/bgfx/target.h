// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  target.h - Render target abstraction for BGFX layer
//
//============================================================

#pragma once

#ifndef __DRAWBGFX_TARGET__
#define __DRAWBGFX_TARGET__

#include <bgfx/bgfx.h>

#include <string>

#include "texturehandleprovider.h"

enum
{
	TARGET_STYLE_GUEST = 0,
	TARGET_STYLE_NATIVE,
	TARGET_STYLE_CUSTOM
};

class bgfx_target : public bgfx_texture_handle_provider
{
public:
	// attachment_count > 1 gives the target that many colour attachments, all of the same format,
	// sharing one depth buffer. A pass bound to it writes gl_FragData[0..N-1]; an input can sample
	// any one of them. Needed where a single pass has to update more state than four channels hold -
	// the vector phosphor pool keeps a peak colour AND a per-channel age, which is six.
	bgfx_target(std::string name, bgfx::TextureFormat::Enum format, uint16_t width, uint16_t height, uint16_t xprescale, uint16_t yprescale,
		uint32_t style, bool double_buffer, bool filter, float scale, uint32_t screen, uint32_t attachment_count = 1);
	bgfx_target(void *handle, uint16_t width, uint16_t height);
	virtual ~bgfx_target();

	void page_flip();
	uint32_t clear(uint32_t view);

	// Getters
	bgfx::FrameBufferHandle     target();
	bgfx::TextureFormat::Enum   format() const { return m_format; }
	std::string                 name() const { return m_name; }
	bool                        double_buffered() const { return m_double_buffer; }
	uint32_t                    style() const { return m_style; }
	bool                        filter() const { return m_filter; }
	float                       scale() const { return m_scale; }
	uint32_t                    screen_index() const { return m_screen; }
	uint16_t                    raw_width() const { return m_width; }
	uint16_t                    raw_height() const { return m_height; }

	// bgfx_texture_handle_provider
	virtual bgfx::TextureHandle texture() const override;
	virtual bgfx::TextureHandle texture(uint32_t attachment) const override;
	virtual uint32_t attachments() const override { return m_attachments; }
	virtual bool is_target() const override { return true; }
	virtual uint16_t width() const override { return m_width * m_xprescale; }
	virtual uint16_t width_margin() const override { return 0; }
	virtual uint16_t height() const override { return m_height * m_yprescale; }
	virtual uint16_t rowpixels() const override { return m_width * m_xprescale; }
	virtual int width_div_factor() const override { return 1; }
	virtual int width_mul_factor() const override { return 1; }

private:
	std::string                 m_name;
	bgfx::TextureFormat::Enum   m_format;
	//bool                      m_readback;

	uint32_t                    m_attachments;
	bgfx::FrameBufferHandle*    m_targets;
	bgfx::TextureHandle*        m_textures;

	uint16_t                    m_width;
	uint16_t                    m_height;
	uint16_t                    m_xprescale;
	uint16_t                    m_yprescale;

	bool                        m_double_buffer;
	uint32_t                    m_style;
	bool                        m_filter;
	float                       m_scale;

	int32_t                     m_screen;

	uint32_t                    m_current_page;

	bool                        m_initialized;

	const uint32_t              m_page_count;
};

#endif // __DRAWBGFX_TARGET__
