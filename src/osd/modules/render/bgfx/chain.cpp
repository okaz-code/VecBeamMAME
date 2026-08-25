// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  chain.cpp - BGFX screen-space post-effect chain
//
//============================================================

#include "chain.h"

#include "emu.h"
#include "render.h"
#include "rendlay.h"
#include "screen.h"

#include <bx/timer.h>

#include "slider.h"
#include "parameter.h"
#include "entryuniform.h"
#include "valueuniform.h"
#include "texturemanager.h"
#include "targetmanager.h"
#include "chainmanager.h"
#include "target.h"
#include "vertex.h"
#include "clear.h"
#include "viewprofile.h"
#include "modules/osdwindow.h"

bgfx_chain::bgfx_chain(
		std::string &&name,
		std::string &&author,
		bool transform,
		target_manager& targets,
		std::vector<bgfx_slider*> &&sliders,
		std::vector<bgfx_parameter*> &&params,
		std::vector<bgfx_chain_entry*> &&entries,
		std::vector<bgfx_target*> &&target_list,
		std::uint32_t screen_index)
	: m_name(std::move(name))
	, m_author(std::move(author))
	, m_transform(transform)
	, m_targets(targets)
	, m_sliders(std::move(sliders))
	, m_params(std::move(params))
	, m_entries(std::move(entries))
	, m_target_list(std::move(target_list))
	, m_current_time(0)
	, m_screen_index(screen_index)
	, m_has_converter(false)
{
	for (bgfx_target* target : m_target_list)
	{
		m_target_map[target->name()] = target;
		m_target_names.push_back(target->name());
	}
}

bgfx_chain::~bgfx_chain()
{
	for (bgfx_slider* slider : m_sliders)
	{
		delete slider;
	}
	for (bgfx_parameter* param : m_params)
	{
		delete param;
	}
	for (bgfx_chain_entry* entry : m_entries)
	{
		delete entry;
	}
	for (bgfx_target* target : m_target_list)
	{
		m_targets.destroy_target(target->name(), m_screen_index);
	}
}

void bgfx_chain::repopulate_targets()
{
	for (size_t i = 0; i < m_target_names.size(); i++)
	{
		bgfx_target* target = m_targets.target(m_screen_index, m_target_names[i]);
		if (target != nullptr) {
			m_target_list[i] = target;
		}
	}
}

uint32_t bgfx_chain::process(chain_manager::screen_prim &prim, int view, int screen, texture_manager& textures, osd_window& window, bool vector_repeat)
{
	screen_device_enumerator screen_iterator(window.machine().root_device());
	screen_device* screen_device = screen_iterator.byindex(screen);

	uint16_t screen_count(window.target()->current_view().visible_screen_count());
	uint16_t screen_width = prim.m_quad_width;
	uint16_t screen_height = prim.m_quad_height;
	uint32_t rotation_type =
		(window.target()->orientation() & ROT90)  == ROT90  ? 1 :
		(window.target()->orientation() & ROT180) == ROT180 ? 2 :
		(window.target()->orientation() & ROT270) == ROT270 ? 3 : 0;
	bool orientation_swap_xy = (window.machine().system().flags & ORIENTATION_SWAP_XY) == ORIENTATION_SWAP_XY;
	bool rotation_swap_xy = (window.target()->orientation() & ORIENTATION_SWAP_XY) == ORIENTATION_SWAP_XY;
	bool swap_xy = orientation_swap_xy ^ rotation_swap_xy;

	// The synthetic vector prim (m_prim == nullptr, see inject_vector_screen) is already
	// window-oriented: the FBO is drawn in window space, so the screen rotation has already
	// been applied to the line geometry. Passing rotation metadata to the shaders here would
	// make them re-apply orientation handling (e.g. fs_distortion swaps u_target_dims by
	// u_swap_xy before comparing against u_quad_dims) and distort the aspect ratio.
	if (prim.m_prim == nullptr)
	{
		rotation_type = 0;
		swap_xy = false;
	}

	float screen_scale_x =  1.0f;
	float screen_scale_y = 1.0f;
	float screen_offset_x = 0.0f;
	float screen_offset_y = 0.0f;
	if (screen_device != nullptr)
	{
		render_container &screen_container = screen_device->container();
		screen_scale_x = 1.0f / screen_container.xscale();
		screen_scale_y = 1.0f / screen_container.yscale();
		screen_offset_x = -screen_container.xoffset();
		screen_offset_y = -screen_container.yoffset();
	}

	int current_view = view;
	for (size_t i = 0; i < m_entries.size(); i++)
	{
		if ((!vector_repeat || vector_repeat_entry(*m_entries[i])) && !m_entries[i]->skip())
		{
			// Label the view so this pass shows up by name in the per-view GPU breakdown
			// (and in a graphics debugger's capture). Which id a pass lands on depends on
			// which optional passes ran ahead of it, hence per submit rather than once.
			bgfx_view_profile::name(uint32_t(current_view), m_entries[i]->name().c_str());
			m_entries[i]->submit(current_view, prim, textures, screen_count, screen_width, screen_height, screen_scale_x, screen_scale_y, screen_offset_x, screen_offset_y,
				rotation_type, swap_xy, screen);
			current_view++;
		}
	}

	m_current_time = bx::getHPCounter();
	static int64_t last = m_current_time;
	const int64_t frameTime = m_current_time - last;
	last = m_current_time;
	const auto freq = double(bx::getHPFrequency());
	const double toMs = 1000.0 / freq;
	const double frameTimeInSeconds = (double)frameTime / 1000000.0;

	for (bgfx_parameter* param : m_params)
	{
		param->tick(frameTimeInSeconds * toMs);
	}

	return uint32_t(current_view - view);
}

bool bgfx_chain::vector_repeat_entry(const bgfx_chain_entry& entry) const
{
	const std::string& name = entry.name();
	return name == "Phosphor"
		|| name == "Phosphor Apply"
		|| name == "add_mglow"
		|| name == "Glow Combine"
		|| name == "Final Blit (unwarped -> screen_hdr)";
}

bool bgfx_chain::supports_vector_repeat() const
{
	if (!m_vector_engine)
		return false;

	bool has_internal_input = false;
	bool has_phosphor = false;
	bool has_phosphor_apply = false;
	bool has_final = false;
	for (const bgfx_chain_entry* entry : m_entries)
	{
		const std::string& name = entry->name();
		has_internal_input |= name == "Initial Blit (linearize, screen -> internal)";
		has_phosphor |= name == "Phosphor";
		has_phosphor_apply |= name == "Phosphor Apply";
		has_final |= name == "add_mglow"
			|| name == "Final Blit (unwarped -> screen_hdr)";
	}

	return has_internal_input && has_phosphor && has_phosphor_apply && has_final
		&& (m_targets.target(m_screen_index, "internal") != nullptr)
		&& (m_targets.target(m_screen_index, "glow_accum") != nullptr);
}

uint32_t bgfx_chain::prepare_vector_repeat(int view, int screen)
{
	uint32_t used_views = 0;
	// A repeat present rebuilds the current phosphor emission in "internal", but it deliberately
	// skips the expensive narrow/wide glow cascade. Preserve glow_accum (and the glow_lo pyramid)
	// from the latest full emulated frame so add_mglow can composite that cached bloom. Clearing
	// glow_accum here made most high-rate presents appear to use the minimal default-vector chain.
	for (const char* name : { "internal" })
	{
		bgfx_target* target = m_targets.target(screen, name);
		if (target == nullptr || target->width() == 0 || target->height() == 0)
			continue;

		const uint16_t clear_view = uint16_t(view + used_views++);
		bgfx_view_profile::name(clear_view, "chain_repeat_clear");
		bgfx::setViewFrameBuffer(clear_view, target->target());
		bgfx::setViewRect(clear_view, 0, 0, target->width(), target->height());
		bgfx::setViewClear(clear_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
		bgfx::touch(clear_view);

		// Match bgfx_chain_entry::submit(): after writing the current page,
		// expose it as the input texture for the following selected pass.
		target->page_flip();
	}
	return used_views;
}

uint32_t bgfx_chain::clear_targets(int view)
{
	uint32_t used_views = 0;
	for (bgfx_target *target : m_target_list)
	{
		if (target != nullptr)
			used_views += target->clear(view + used_views);
	}
	return used_views;
}

uint32_t bgfx_chain::applicable_passes(bool vector_repeat)
{
	int applicable_passes = 0;
	for (bgfx_chain_entry* entry : m_entries)
	{
		if ((!vector_repeat || vector_repeat_entry(*entry)) && !entry->skip())
		{
			applicable_passes++;
		}
	}

	return applicable_passes;
}

void bgfx_chain::insert_effect(uint32_t index, bgfx_effect *effect, const bool apply_tint, std::string name, std::string source, chain_manager &chains)
{
	auto *clear = new clear_state(BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0, 1.0f, 0);
	std::vector<bgfx_suppressor*> suppressors;

	std::vector<bgfx_input_pair*> inputs;
	std::vector<std::string> available_textures;
	inputs.push_back(new bgfx_input_pair(0, "s_tex", source,  available_textures, "", chains, m_screen_index));
	inputs.push_back(new bgfx_input_pair(1, "s_pal", "palette", available_textures, "", chains, m_screen_index));

	std::vector<bgfx_entry_uniform*> uniforms;
	float value = 1.0f;
	float values[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("s_tex", bgfx::UniformType::Sampler), &value, 1));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("s_pal", bgfx::UniformType::Sampler), &value, 1));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_tex_size0", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_tex_size1", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_inv_tex_size0", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_inv_tex_size1", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_tex_bounds0", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_tex_bounds1", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_inv_tex_bounds0", bgfx::UniformType::Vec4), values, 4));
	uniforms.push_back(new bgfx_value_uniform(new bgfx_uniform("u_inv_tex_bounds1", bgfx::UniformType::Vec4), values, 4));

	m_entries.insert(m_entries.begin() + index, new bgfx_chain_entry(name, effect, clear, suppressors, inputs, uniforms, m_targets, "screen", apply_tint));

	const uint32_t screen_width = chains.targets().width(TARGET_STYLE_GUEST, m_screen_index);
	const uint32_t screen_height = chains.targets().height(TARGET_STYLE_GUEST, m_screen_index);
	m_targets.destroy_target("screen", m_screen_index);
	m_targets.create_target("screen", bgfx::TextureFormat::BGRA8, screen_width, screen_height, 1, 1, TARGET_STYLE_GUEST, true, false, 1, m_screen_index);
}
