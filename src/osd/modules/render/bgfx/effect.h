// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  effect.h - BGFX shader material to be applied to a mesh
//
//============================================================

#ifndef MAME_RENDER_BGFX_EFFECT_H
#define MAME_RENDER_BGFX_EFFECT_H

#pragma once

#include <bgfx/bgfx.h>

#include <map>
#include <memory>
#include <string>
#include <vector>


class bgfx_uniform;

class bgfx_effect
{
public:
	bgfx_effect(std::string &&name, uint64_t state, bgfx::ShaderHandle vertex_shader, bgfx::ShaderHandle fragment_shader, std::vector<std::unique_ptr<bgfx_uniform> > &uniforms);
	~bgfx_effect();

	void submit(int view, uint64_t blend = ~0ULL);
	bgfx_uniform *uniform(const std::string &name);
	bool is_valid() const { return m_program_handle.idx != bgfx::kInvalidHandle; }
	// Restores every uniform on this effect to its JSON-declared default. This effect INSTANCE is
	// shared/cached by name across every chain that references it (effect_manager::get_or_load_effect),
	// and a chain pass only ever touches the uniforms ITS OWN JSON explicitly lists - any uniform this
	// pass doesn't rebind otherwise keeps whatever value a DIFFERENT pass (potentially from a totally
	// different, currently-inactive chain) last set on this shared instance, with nothing to reset it
	// until the whole app restarts. Called once per pass submit, before that pass's own bindings are
	// applied, so those bindings still take effect - this only fills in what a pass leaves unspecified.
	void reset_uniforms_to_default();

private:
	std::string                          m_name;
	uint64_t                             m_state;
	bgfx::ProgramHandle                  m_program_handle;
	std::map<std::string, std::unique_ptr<bgfx_uniform> > m_uniforms;
};

#endif // MAME_RENDER_BGFX_EFFECT_H
