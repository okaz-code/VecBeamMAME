// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  uniform.h - Shader uniform abstraction for BGFX layer
//
//============================================================

#ifndef MAME_RENDER_BGFX_UNIFORM_H
#define MAME_RENDER_BGFX_UNIFORM_H

#pragma once

#include <bgfx/bgfx.h>

#include <string>

class bgfx_uniform
{
public:
	bgfx_uniform(std::string &&name, bgfx::UniformType::Enum type);
	virtual ~bgfx_uniform();

	virtual void upload();

	void create();

	// Getters
	const std::string &name() { return m_name; }
	bgfx::UniformType::Enum type() const { return m_type; }
	bgfx::UniformHandle handle() const { return m_handle; }

	// Setters
	bgfx_uniform* set(float* value);
	bgfx_uniform* set_int(int value);
	bgfx_uniform* set_mat3(float* value);
	bgfx_uniform* set_mat4(float* value);
	bgfx_uniform* set(void* data, size_t size);

	// Captures the effect JSON's declared "values" as this uniform's default (called once, right
	// after construction, by uniform_reader - see its comment for why this exists).
	void set_default(void* data, size_t size);
	// Restores the current value to the captured default. See bgfx_effect::reset_uniforms_to_default.
	void reset_to_default();

	static size_t get_size_for_type(bgfx::UniformType::Enum type);

protected:
	bgfx::UniformHandle     m_handle;
	std::string             m_name;
	bgfx::UniformType::Enum m_type;
	uint8_t*                m_data;
	uint8_t*                m_default_data;
	size_t                  m_data_size;
};

#endif // MAME_RENDER_BGFX_UNIFORM_H
