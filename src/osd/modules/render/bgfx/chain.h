// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  chain.h - BGFX screen-space post-effect chain
//
//============================================================

#ifndef MAME_RENDER_BGFX_CHAIN_H
#define MAME_RENDER_BGFX_CHAIN_H

#pragma once

#include "chainentry.h"

#include <string>
#include <vector>
#include <map>

class bgfx_slider;
class bgfx_parameter;
class texture_manager;
class target_manager;
class bgfx_target;
class osd_window;

class bgfx_chain
{
public:
	bgfx_chain(std::string &&name, std::string &&author, bool transform, target_manager& targets, std::vector<bgfx_slider*> &&sliders, std::vector<bgfx_parameter*> &&params, std::vector<bgfx_chain_entry*> &&entries, std::vector<bgfx_target*> &&target_list, uint32_t screen_index);
	~bgfx_chain();

	uint32_t process(chain_manager::screen_prim &prim, int view, int screen, texture_manager& textures, osd_window &window, bool vector_repeat = false);
	uint32_t prepare_vector_repeat(int view, int screen);
	bool supports_vector_repeat() const;
	void repopulate_targets();

	// Getters
	const std::string &name() const { return m_name; }
	std::vector<bgfx_slider*>& sliders() { return m_sliders; }
	std::vector<bgfx_chain_entry*>& entries() { return m_entries; }
	uint32_t applicable_passes(bool vector_repeat = false);
	bool transform() const { return m_transform; }
	bool has_converter() const { return m_has_converter; }
	bool has_adjuster() const { return m_has_adjuster; }
	// Opt-in flag from the chain JSON's top-level "vector_engine": "analytic" key. The OSD renderer
	// routes vector LINE primitives through its analytic beam FBO path (phosphor pool inputs,
	// glow / no-persist FBOs, beam-energy model) only while a chain declaring this is active;
	// chains without it keep the stock render path untouched.
	bool vector_engine() const { return m_vector_engine; }

	// Setters
	void set_has_converter(bool has_converter) { m_has_converter = has_converter; }
	void set_has_adjuster(bool has_adjuster) { m_has_adjuster = has_adjuster; }
	void set_vector_engine(bool vector_engine) { m_vector_engine = vector_engine; }
	void insert_effect(uint32_t index, bgfx_effect *effect, const bool apply_tint, std::string name, std::string source, chain_manager &chains);

private:
	std::string                         m_name;
	std::string                         m_author;
	bool                                m_transform;
	target_manager&                     m_targets;
	std::vector<bgfx_slider*>           m_sliders;
	std::vector<bgfx_parameter*>        m_params;
	std::vector<bgfx_chain_entry*>      m_entries;
	std::vector<bgfx_target*>           m_target_list;
	std::vector<std::string>            m_target_names;
	std::map<std::string, bgfx_target*> m_target_map;
	int64_t                             m_current_time;
	uint32_t                            m_screen_index;
	bool                                m_has_converter;
	bool                                m_has_adjuster;
	bool                                m_vector_engine = false;

	bool vector_repeat_entry(const bgfx_chain_entry& entry) const;
};

#endif // MAME_RENDER_BGFX_CHAIN_H
