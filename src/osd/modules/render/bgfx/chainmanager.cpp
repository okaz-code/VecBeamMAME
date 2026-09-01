// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  chainmanager.cpp - BGFX shader chain manager
//
//  Provides loading for BGFX shader effect chains, defined
//  by chain.h and read by chainreader.h
//
//============================================================

#include "chainmanager.h"

#include <bx/readerwriter.h>
#include <bx/file.h>

#include "emu.h"
#include "emucore.h"
#include "render.h"
#include "screen.h"
#include "../frontend/mame/ui/slider.h"

#include "modules/lib/osdobj_common.h"
#include "modules/osdwindow.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include "bgfxutil.h"

#include "chain.h"
#include "chainentry.h"
#include "chainreader.h"
#include "fbotextureprovider.h"
#include "slider.h"
#include "target.h"
#include "texture.h"

#include "sliderdirtynotifier.h"

#include "util/path.h"
#include "util/unicode.h"
#include "util/xmlfile.h"

#include "osdcore.h"
#include "osdfile.h"

#include <algorithm>
#include <array>
#include <locale>


using namespace rapidjson;

chain_manager::screen_prim::screen_prim(render_primitive *prim)
{
	m_prim = prim;
	m_screen_width = uint16_t(floorf(prim->get_full_quad_width() + 0.5f));
	m_screen_height = uint16_t(floorf(prim->get_full_quad_height() + 0.5f));
	m_quad_width = uint16_t(floorf(prim->get_quad_width() + 0.5f));
	m_quad_height = uint16_t(floorf(prim->get_quad_height() + 0.5f));
	m_content_width = m_quad_width;
	m_content_height = m_quad_height;
	m_tex_width = prim->texture.width;
	m_tex_height = prim->texture.height;
	m_rowpixels = prim->texture.rowpixels;
	m_palette_length = prim->texture.palette_length;
	m_flags = prim->flags;
}

chain_manager::chain_manager(
		running_machine& machine,
		const osd_options& options,
		texture_manager& textures,
		target_manager& targets,
		effect_manager& effects,
		uint32_t window_index,
		slider_dirty_notifier& slider_notifier,
		uint16_t user_prescale,
		uint16_t max_prescale_size)
	: m_machine(machine)
	, m_options(options)
	, m_textures(textures)
	, m_targets(targets)
	, m_effects(effects)
	, m_window_index(window_index)
	, m_user_prescale(user_prescale)
	, m_max_prescale_size(max_prescale_size)
	, m_slider_notifier(slider_notifier)
	, m_screen_count(0)
	, m_default_chain_index(-1)
{
	m_converters.clear();
	// Determine the game type (vector vs raster) once in the constructor and cache it
	detect_vector_game();
	refresh_available_chains();
	parse_chain_selections(options.bgfx_screen_chains());
	init_texture_converters();
}

namespace
{
	template <std::size_t N>
	bool matches_system_or_parent(game_driver const &system, std::array<std::string_view, N> const &names)
	{
		std::string_view const name(system.name);
		std::string_view const parent(system.parent);
		return std::find(names.begin(), names.end(), name) != names.end()
			|| std::find(names.begin(), names.end(), parent) != names.end();
	}
}

// Scan the machine's screen devices and classify known vector monitor families.  MAME has no
// general colour/monochrome flag for vector screens, and sampling the first rendered frames is not
// reliable (many colour games boot into a monochrome scene), so keep the small hardware-family
// classification explicit.  Unknown future vector systems deliberately retain default-vector.
void chain_manager::detect_vector_game()
{
	m_is_vector_game = false;
	m_vector_monitor_type = vector_monitor_type::UNKNOWN;
	screen_device_enumerator screens(m_machine.root_device());
	for (screen_device& s : screens)
	{
		if (s.screen_type() == SCREEN_TYPE_VECTOR)
		{
			m_is_vector_game = true;
			break;
		}
	}

	if (!m_is_vector_game)
		return;

	game_driver const &system = m_machine.system();
	static constexpr std::array vectrex_systems = { std::string_view("vectrex"), std::string_view("raaspec") };
	static constexpr std::array color_systems = {
		std::string_view("aztarac"), std::string_view("boxingb"), std::string_view("bwidow"),
		std::string_view("cchasm"), std::string_view("elim2"), std::string_view("gravitar"),
		std::string_view("mhavoc"), std::string_view("qb3"), std::string_view("quantum"),
		std::string_view("spacduel"), std::string_view("spacfury"), std::string_view("startrek"),
		std::string_view("starwars"), std::string_view("esb"), std::string_view("tacscan"),
		std::string_view("tempest"), std::string_view("tomcat"), std::string_view("topgunnr"),
		std::string_view("wotwc"), std::string_view("zektor")
	};
	static constexpr std::array monochrome_systems = {
		std::string_view("armora"), std::string_view("astdelux"), std::string_view("asteroid"),
		std::string_view("barrier"), std::string_view("bradley"), std::string_view("bzone"),
		std::string_view("demon"), std::string_view("llander"), std::string_view("omegrace"),
		std::string_view("redbaron"), std::string_view("ripoff"), std::string_view("solarq"),
		std::string_view("spacewar"), std::string_view("speedfrk"), std::string_view("starcas"),
		std::string_view("starhawk"), std::string_view("sundance"), std::string_view("tailg"),
		std::string_view("tek4051"), std::string_view("warrior"), std::string_view("wotw")
	};

	if (matches_system_or_parent(system, vectrex_systems))
		m_vector_monitor_type = vector_monitor_type::VECTREX;
	else if (matches_system_or_parent(system, color_systems))
		m_vector_monitor_type = vector_monitor_type::COLOR;
	else if (matches_system_or_parent(system, monochrome_systems))
		m_vector_monitor_type = vector_monitor_type::MONOCHROME;
}

std::string_view chain_manager::preferred_vector_chain() const
{
	switch (m_vector_monitor_type)
	{
	case vector_monitor_type::COLOR:      return "vector-color";
	case vector_monitor_type::MONOCHROME: return "vector-monochrome";
	case vector_monitor_type::VECTREX:    return "vector-vectrex";
	default:                              return {};
	}
}

std::string_view chain_manager::canonical_chain_name(std::string_view name)
{
	if (name == "vector-color-balanced")
		return "vector-color";
	if (name == "vector-monochrome-balanced_1" || name == "vector-monochrome-balanced_2")
		return "vector-monochrome";
	if (name == "vector-vectrex-balanced")
		return "vector-vectrex";
	return name;
}

int32_t chain_manager::find_chain_index(std::string_view name) const
{
	for (std::size_t i = 0; i < m_available_chains.size(); ++i)
		if (m_available_chains[i].m_name == name)
			return int32_t(i);
	return -1;
}

int32_t chain_manager::find_vector_fallback_index(bool include_profile_chain) const
{
	if (include_profile_chain)
	{
		std::string_view const preferred = preferred_vector_chain();
		if (!preferred.empty())
		{
			int32_t const preferred_index = find_chain_index(preferred);
			if (preferred_index >= 0 && m_available_chains[preferred_index].m_is_vector)
				return preferred_index;
		}
	}

	int32_t const safe_index = find_chain_index("default-vector");
	return safe_index >= 0 && m_available_chains[safe_index].m_is_vector ? safe_index : -1;
}

// Scan m_available_chains and build the list of absolute indices compatible with the current
// game type (m_is_vector_game). CHAIN_NONE (0) is always included.
void chain_manager::rebuild_compat_chain_indices()
{
	m_compat_chain_indices.clear();
	for (size_t i = 0; i < m_available_chains.size(); i++)
	{
		if (i == CHAIN_NONE || m_available_chains[i].m_is_vector == m_is_vector_game)
			m_compat_chain_indices.push_back(i);
	}
}

chain_manager::~chain_manager()
{
	destroy_chains();
}

void chain_manager::init_texture_converters()
{
	m_converters.push_back(nullptr);
	m_converters.push_back(m_effects.get_or_load_effect(m_options, "misc/texconv_palette16"));
	m_converters.push_back(m_effects.get_or_load_effect(m_options, "misc/texconv_rgb32"));
	m_converters.push_back(nullptr);
	m_converters.push_back(m_effects.get_or_load_effect(m_options, "misc/texconv_yuy16"));
	m_adjuster = m_effects.get_or_load_effect(m_options, "misc/bcg_adjust");
}

void chain_manager::get_default_chain_info(std::string &out_chain_name, int32_t &out_chain_index)
{
	if (m_default_chain_index == -1)
	{
		out_chain_index = CHAIN_NONE;
		out_chain_name = "";
		return;
	}

	out_chain_index = m_default_chain_index;
	// For a vector game, m_default_chain_index is the absolute index of the first vector chain.
	// Look up the correct name from m_available_chains (the old code returned a hardcoded "default").
	out_chain_name = m_available_chains[m_default_chain_index].m_name;
	return;
}

void chain_manager::refresh_available_chains()
{
	m_available_chains.clear();
	m_available_chains.emplace_back("none", "");

	find_available_chains(util::path_concat(m_options.bgfx_path(), "chains"), "");
	std::collate<wchar_t> const &coll = std::use_facet<std::collate<wchar_t> >(std::locale());
	std::sort(
			m_available_chains.begin(),
			m_available_chains.end(),
			[&coll] (chain_desc const &x, chain_desc const &y) -> bool
			{
				if (x.m_name == "none")
					return y.m_name != "none";
				else if (y.m_name == "none")
					return false;
				else if (x.m_name == "default")
					return y.m_name != "default";
				else if (y.m_name == "default")
					return false;
				else if (x.m_name == "default-vector")
					return y.m_name != "default-vector";
				else if (y.m_name == "default-vector")
					return false;
				std::wstring const xstr = wstring_from_utf8(x.m_name);
				std::wstring const ystr = wstring_from_utf8(y.m_name);
				return coll.compare(xstr.data(), xstr.data() + xstr.size(), ystr.data(), ystr.data() + ystr.size()) < 0;
			});

	m_default_chain_index = -1;
	if (m_is_vector_game)
	{
		// Prefer the calibrated chain for the detected monitor family.  If it is absent, use the
		// deliberately minimal default-vector chain.  Unknown vector hardware also starts there.
		m_default_chain_index = find_vector_fallback_index(true);
		if (m_default_chain_index == -1)
		{
			// Last-resort compatibility for incomplete third-party BGFX installations that have a
			// vector chain but neither the calibrated chain nor default-vector.
			for (size_t i = 0; i < m_available_chains.size(); i++)
			{
				if (m_available_chains[i].m_is_vector)
				{
					m_default_chain_index = int32_t(i);
					break;
				}
			}
		}

		char const *profile = "unknown";
		switch (m_vector_monitor_type)
		{
		case vector_monitor_type::COLOR:      profile = "color"; break;
		case vector_monitor_type::MONOCHROME: profile = "monochrome"; break;
		case vector_monitor_type::VECTREX:    profile = "Vectrex"; break;
		default: break;
		}
		osd_printf_verbose("BGFX: vector monitor profile %s, default chain '%s'\n",
			profile,
			m_default_chain_index >= 0 ? m_available_chains[m_default_chain_index].m_name.c_str() : "none");
	}
	else
	{
		for (size_t i = 0; i < m_available_chains.size(); i++)
		{
			if (m_available_chains[i].m_name == "default")
			{
				m_default_chain_index = int32_t(i);
				break;
			}
		}
	}

	// rebuild the compat index list (indices shift on every refresh)
	rebuild_compat_chain_indices();

	destroy_unloaded_chains();
}

void chain_manager::destroy_unloaded_chains()
{
	// O(shaders*available_chains), but we don't care because asset reloading happens rarely
	for (int i = 0; i < m_chain_names.size(); i++)
	{
		const std::string &name = m_chain_names[i];
		if (name.length() > 0)
		{
			for (chain_desc desc : m_available_chains)
			{
				if (desc.m_name == name)
				{
					delete m_screen_chains[i];
					m_screen_chains[i] = nullptr;
					get_default_chain_info(m_chain_names[i], m_current_chain[i]);
					break;
				}
			}
		}
	}
}

void chain_manager::find_available_chains(std::string_view root, std::string_view path)
{
	osd::directory::ptr directory = osd::directory::open(path.empty() ? std::string(root) : util::path_concat(root, path));
	if (directory)
	{
		for (const osd::directory::entry *entry = directory->read(); entry; entry = directory->read())
		{
			if (entry->type == osd::directory::entry::entry_type::FILE)
			{
				const std::string_view name(entry->name);
				const std::string_view extension(".json");

				// Does the name has at least one character in addition to ".json"?
				if (name.length() > extension.length())
				{
					size_t start = name.length() - extension.length();
					const std::string_view test_segment = name.substr(start, extension.length());

					// Does it end in .json?
					if (test_segment == extension)
					{
						// Lightweight JSON pre-scan to extract the "screen_type" tag
						std::string chain_name(name.substr(0, start));
						std::string chain_subdir(path);
						bool is_vector = false;
						{
							std::string full_relpath = chain_subdir.empty()
								? (chain_name + ".json")
								: util::path_concat(chain_subdir, chain_name + ".json");
							std::string full_abspath = util::path_concat(std::string(root), full_relpath);
							bx::FileReader rdr;
							if (bx::open(&rdr, full_abspath.c_str()))
							{
								int32_t sz = bx::getSize(&rdr);
								if (sz > 0)
								{
									std::unique_ptr<char[]> buf(new (std::nothrow) char[sz + 1]);
									if (buf)
									{
										bx::ErrorAssert err;
										bx::read(&rdr, buf.get(), sz, &err);
										buf[sz] = 0;
										Document doc;
										doc.Parse<kParseCommentsFlag>(buf.get());
										if (!doc.HasParseError() && doc.HasMember("screen_type") && doc["screen_type"].IsString())
										{
											if (std::string_view(doc["screen_type"].GetString()) == "vector")
												is_vector = true;
										}
									}
								}
								bx::close(&rdr);
							}
						}
						m_available_chains.emplace_back(std::move(chain_name), std::move(chain_subdir), is_vector);
					}
				}
			}
			else if (entry->type == osd::directory::entry::entry_type::DIR)
			{
				const std::string_view name = entry->name;
				if ((name != ".") && (name != ".."))
				{
					if (path.empty())
						find_available_chains(root, name);
					else
						find_available_chains(root, util::path_concat(path, name));
				}
			}
		}
	}
}

std::unique_ptr<bgfx_chain> chain_manager::load_chain(std::string name, uint32_t screen_index)
{
	if (name.length() < 5 || (name.compare(name.length() - 5, 5, ".json") != 0))
	{
		name += ".json";
	}
	const std::string path = util::path_concat(m_options.bgfx_path(), "chains", name);

	bx::FileReader reader;
	if (!bx::open(&reader, path.c_str()))
	{
		osd_printf_warning("Unable to open chain file %s, falling back to no post processing\n", path);
		return nullptr;
	}

	const int32_t size(bx::getSize(&reader));

	bx::ErrorAssert err;
	std::unique_ptr<char []> data(new (std::nothrow) char [size + 1]);
	if (!data)
	{
		osd_printf_error("Out of memory reading chain file %s\n", path);
		bx::close(&reader);
		return nullptr;
	}

	bx::read(&reader, reinterpret_cast<void*>(data.get()), size, &err);
	bx::close(&reader);
	data[size] = 0;

	Document document;
	document.Parse<kParseCommentsFlag>(data.get());
	data.reset();

	if (document.HasParseError())
	{
		std::string error(GetParseError_En(document.GetParseError()));
		osd_printf_warning("Unable to parse chain %s. Errors returned:\n%s\n", path, error);
		return nullptr;
	}

	std::unique_ptr<bgfx_chain> chain = chain_reader::read_from_value(document, name + ": ", *this, screen_index, m_user_prescale, m_max_prescale_size);

	if (!chain)
	{
		osd_printf_warning("Unable to load chain %s, falling back to no post processing\n", path);
		return nullptr;
	}

	return chain;
}

void chain_manager::parse_chain_selections(std::string_view chain_str)
{
	std::vector<std::string_view> chain_names = split_option_string(chain_str);

	if (chain_names.empty())
		chain_names.push_back("default");

	while (m_current_chain.size() < chain_names.size())
	{
		m_screen_chains.emplace_back(nullptr);
		m_chain_names.emplace_back();
		m_current_chain.push_back(CHAIN_NONE);
	}

	for (size_t index = 0; index < chain_names.size(); index++)
	{
		std::string_view const requested_name = chain_names[index];
		std::string_view const resolved_name = canonical_chain_name(requested_name);
		size_t chain_index = 0;
		for (chain_index = 0; chain_index < m_available_chains.size(); chain_index++)
		{
			if (m_available_chains[chain_index].m_name == resolved_name)
				break;
		}

		if (chain_index < m_available_chains.size())
		{
			// Verify screen_type compatibility; fall back if incompatible.
			const bool compat = (chain_index == CHAIN_NONE) ||
				(m_available_chains[chain_index].m_is_vector == m_is_vector_game);
			if (!compat)
			{
				// Informational only: the default selection ("default") is a raster chain, so vector
				// games routinely fall back here. Keep it at verbose level to avoid console noise.
				osd_printf_verbose("BGFX: chain '%s' is not compatible with %s game; using fallback\n",
					std::string(requested_name).c_str(),
					m_is_vector_game ? "vector" : "raster");
				chain_index = m_default_chain_index >= 0 ? size_t(m_default_chain_index) : CHAIN_NONE;
			}
			m_current_chain[index] = chain_index;
			m_chain_names[index] = m_available_chains[chain_index].m_name;
		}
		else
		{
			// A stale or unavailable named chain follows the same monitor-specific fallback path.
			// An explicit "none" is found above and is therefore never overridden here.
			int32_t const fallback = m_default_chain_index;
			m_current_chain[index] = fallback >= 0 ? fallback : int32_t(CHAIN_NONE);
			m_chain_names[index] = fallback >= 0 ? m_available_chains[fallback].m_name : "";
			osd_printf_warning("BGFX: chain '%s' is unavailable; using '%s'\n",
				std::string(requested_name).c_str(), m_chain_names[index].empty() ? "none" : m_chain_names[index].c_str());
		}
	}
}

std::vector<std::string_view> chain_manager::split_option_string(std::string_view chain_str) const
{
	std::vector<std::string_view> chain_names;

	const uint32_t length = chain_str.length();
	uint32_t win = 0;
	uint32_t last_start = 0;
	for (uint32_t i = 0; i <= length; i++)
	{
		if (i == length || (chain_str[i] == ',') || (chain_str[i] == ':'))
		{
			if ((win == 0) || (win == m_window_index))
			{
				// treat an empty string as equivalent to "default"
				if (i > last_start)
					chain_names.push_back(chain_str.substr(last_start, i - last_start));
				else
					chain_names.push_back("default");
			}

			last_start = i + 1;
			if ((i < length) && (chain_str[i] == ':'))
			{
				// no point walking the rest of the string if this was our window
				if (win == m_window_index)
					break;

				// don't use first for all if more than one window is specified
				chain_names.clear();
				win++;
			}
		}
	}

	return chain_names;
}

void chain_manager::load_chains()
{
	for (size_t chain = 0; chain < m_current_chain.size() && chain < m_screen_chains.size(); chain++)
	{
		if (m_current_chain[chain] != CHAIN_NONE)
		{
			chain_desc& desc = m_available_chains[m_current_chain[chain]];
			m_chain_names[chain] = desc.m_name;
			m_screen_chains[chain] = load_chain(util::path_concat(desc.m_path, desc.m_name), uint32_t(chain)).release();
			if (!m_screen_chains[chain] && m_is_vector_game && desc.m_name != "default-vector")
			{
				int32_t const safe_index = find_vector_fallback_index(false);
				if (safe_index >= 0 && safe_index != m_current_chain[chain])
				{
					chain_desc &safe = m_available_chains[safe_index];
					osd_printf_warning("BGFX: chain '%s' failed to load; using '%s'\n", desc.m_name.c_str(), safe.m_name.c_str());
					m_current_chain[chain] = safe_index;
					m_chain_names[chain] = safe.m_name;
					m_screen_chains[chain] = load_chain(util::path_concat(safe.m_path, safe.m_name), uint32_t(chain)).release();
				}
			}
		}
	}

	// Overlay the HDR auto-config values on the freshly-created sliders. Doing it here (and only
	// here) makes them behave as computed defaults: the cfg restore that follows on the first frame,
	// and restore_slider_settings() across live reloads, both overwrite them as usual.
	// Reset the macro bookkeeping BEFORE the HDR auto-config: apply_hdr_auto() ends by running the
	// macros itself, and the baseline captured there must survive.
	m_macro_last.clear();
	m_macro_imported.clear();
	m_macro_base.clear();
	apply_hdr_auto();
	// Macro output is a computed default too, so a cfg entry for a detail slider (restored on the
	// first frame) still wins.
	apply_macros(true);
}

static bool slider_matches_imported_value(const bgfx_slider &slider, float value)
{
	// bgfx_slider::import snaps a float to the slider's UI step. Compare with that snapped value,
	// not the unsnapped calculation (for example 1390 / 500 = 2.78 imports as 2.80 at step 0.05).
	const float step = std::max(slider.step_value(), 1.0e-6f);
	const float snapped = std::round(value / step) * step;
	return std::abs(slider.value() - snapped) < step * 0.25f;
}

// Does this macro target name address this detail slider?  A bare target name drives every
// component the slider has (vec2 / colour), so one "Defocus" macro can move defocus X and Y
// together.
static bool macro_target_matches(const bgfx_slider::macro_target &target, const bgfx_slider &dest)
{
	if (!target.all_components)
		return dest.name() == target.name;
	return dest.name().compare(0, target.name.size(), target.name) == 0
		&& dest.name().size() == target.name.size() + 1
		&& dest.name().back() >= '0' && dest.name().back() <= '2';
}

void chain_manager::apply_macros(bool force)
{
	for (bgfx_chain *chain : m_screen_chains)
	{
		if (chain == nullptr)
			continue;

		// Collect the macros and note which of them moved.  A detail slider may be claimed by more
		// than one macro - Edge Defocus is SCALED by [M] Defocus and GATED by [M] Monitor/Glass Sim -
		// so its value has to be composed from every macro that targets it, not just from the one
		// that happens to have moved.  Anything the moved macros touch is recomputed in full;
		// everything else is left alone, so a hand edit in the Advanced list survives until a macro
		// that actually owns that slider is moved.
		std::vector<bgfx_slider *> macros;
		std::vector<bgfx_slider *> dirty;
		for (bgfx_slider *macro : chain->sliders())
		{
			if (!macro->is_macro())
				continue;
			macros.push_back(macro);
			const auto last = m_macro_last.find(macro->name());
			if (!force && last != m_macro_last.end() && last->second == macro->value())
				continue;
			m_macro_last[macro->name()] = macro->value();
			for (const bgfx_slider::macro_target &target : macro->macro_targets())
			{
				bool found = false;
				for (bgfx_slider *candidate : chain->sliders())
				{
					if (!macro_target_matches(target, *candidate))
						continue;
					found = true;
					if (std::find(dirty.begin(), dirty.end(), candidate) == dirty.end())
						dirty.push_back(candidate);
				}
				if (!found)
					osd_printf_verbose("BGFX: macro '%s' targets unknown slider '%s'\n",
						macro->name().c_str(), target.name.c_str());
			}
		}
		if (dirty.empty())
			continue;

		for (bgfx_slider *dest : dirty)
		{
			// Capture the pre-macro value once; everything scales from that, not from the JSON
			// default, so an auto-derived target (beam_peak_nits) keeps its derivation.
			const auto base_it = m_macro_base.find(dest->name());
			const float base = (base_it != m_macro_base.end())
					? base_it->second
					: (m_macro_base[dest->name()] = dest->value());

			// SCALE and ENABLE contribute factors, CURVE contributes an absolute level; a slider on
			// both a curve macro and a scale/enable macro ends up as curve(x) * factors.
			float level = base;
			float factor = 1.0f;
			for (bgfx_slider *macro : macros)
			{
				const float value = macro->value();
				for (const bgfx_slider::macro_target &target : macro->macro_targets())
				{
					if (!macro_target_matches(target, *dest))
						continue;
					switch (target.mode)
					{
						case bgfx_slider::MACRO_SCALE:
							factor *= value;
							break;
						case bgfx_slider::MACRO_ENABLE:
							factor *= (value > 0.5f) ? 1.0f : 0.0f;
							break;
						case bgfx_slider::MACRO_CURVE:
						{
							// piecewise linear, clamped at both ends
							level = target.ys.front();
							for (size_t i = 1; i < target.xs.size(); i++)
							{
								if (value <= target.xs[i] || i == target.xs.size() - 1)
								{
									const float x0 = target.xs[i - 1], x1 = target.xs[i];
									const float y0 = target.ys[i - 1], y1 = target.ys[i];
									const float t = (x1 > x0) ? std::clamp((value - x0) / (x1 - x0), 0.0f, 1.0f) : 0.0f;
									level = y0 + (y1 - y0) * t;
									break;
								}
							}
							break;
						}
					}
				}
			}

			const float derived = std::clamp(level * factor, dest->min_value(), dest->max_value());
			dest->import(derived);
			m_macro_imported[dest->name()] = dest->value();
			// The HDR auto-config refuses to update a slider that no longer matches what IT last
			// wrote (that is how it protects user edits). A macro scaling the auto-derived peak is
			// not a user edit, so move that baseline with it - otherwise a later display change
			// would silently stop re-deriving the peak.
			if (dest->name() == "beam_peak_nits0")
				m_hdr_last_auto_beam = dest->value();
			else if (dest->name() == "hdr_rolloff_max0")
				m_hdr_last_auto_rolloff = dest->value();
			// %g, not %.4f: the glow sliders calibrate around 1e-4, where four decimals cannot
			// tell an exact value from one the UI step rounded off.
			osd_printf_verbose("BGFX: macros -> %s = %.6g (base %.6g)\n",
				dest->name().c_str(), dest->value(), base);
		}
	}
}

void chain_manager::apply_hdr_auto()
{
	if ((!m_edr_relative_auto && m_hdr_display_peak <= 0.0f) || !m_options.bgfx_hdr())
		return;

	// Prefer a stable 500-nit normal-vector target when a display peak is known and the panel can keep
	// the target below the established 85% safety ceiling. macOS EDR auto has no absolute nits, so its
	// peak arrives as headroom * reference white - the nominal scale the EDR present shader's dynamic
	// ceiling and the HDR diagnostics already work in - and "500 nits" there means 2.5x SDR white
	// rather than a physical figure. A lower-peak panel, or a peak that is not resolved yet (macOS
	// before the first EDR frame), retains the previous 1.65 * paper-white calibration. Additive
	// crossings/overload then use hdr_rolloff_max to approach the display peak.
	const float paper_white = std::max(1.0f, m_hdr_paper_white);
	const float peak = m_hdr_display_peak;
	const float previous_desired_beam = m_edr_relative_auto
		? 1.65f * paper_white
		: std::min(1.65f * paper_white, 0.85f * peak);
	constexpr float preferred_beam_nits = 500.0f;
	// Roll-off room kept above the preferred beam for additive crossings and overload. 1.0 applies the
	// 85% safety ceiling alone (a panel whose peak only just clears the target still takes the 500-nit
	// beam); raise it if crossings read as compressed on such a panel.
	constexpr float preferred_beam_reserve = 1.0f;
	const bool preferred_beam_has_headroom = peak > 0.0f
		&& preferred_beam_nits * preferred_beam_reserve <= 0.85f * peak;
	const float desired_beam = preferred_beam_has_headroom
		? preferred_beam_nits
		: previous_desired_beam;
	const float beam = std::clamp(std::round(desired_beam / 10.0f) * 10.0f, 80.0f, 2000.0f);
	const float rmax = m_edr_relative_auto ? 0.0f : std::clamp(peak / beam, 1.1f, 8.0f);
	const float previous_beam = m_hdr_last_auto_beam;
	const float previous_rmax = m_hdr_last_auto_rolloff;

	bool applied = false;
	for (size_t screen = 0; screen < m_screen_chains.size(); screen++)
	{
		bgfx_chain *chain = m_screen_chains[screen];
		if (chain == nullptr)
			continue;
		const std::string beam_name = "beam_peak_nits" + std::to_string(screen);
		const std::string rmax_name = "hdr_rolloff_max" + std::to_string(screen);
		for (bgfx_slider *slider : chain->sliders())
		{
			if (slider->name() == beam_name)
			{
				// A display move updates hardware-derived defaults, but must not replace a value restored
				// from cfg or edited live. Values still equal to the preceding auto result remain automatic.
				if (!m_hdr_live_refresh || previous_beam <= 0.0f || slider_matches_imported_value(*slider, previous_beam))
				{
					slider->import(beam);
					applied = true;
				}
			}
			else if (slider->name() == rmax_name)
			{
				// Relative EDR auto must not rewrite the artistic/user ceiling. Current hardware
				// headroom independently constrains that value in the present shader.
				if (m_edr_relative_auto)
					continue;
				if (!m_hdr_live_refresh || previous_rmax <= 0.0f || slider_matches_imported_value(*slider, previous_rmax))
				{
					slider->import(rmax);
					applied = true;
				}
			}
		}
	}
	m_hdr_last_auto_beam = beam;
	m_hdr_last_auto_rolloff = rmax;
	// The values just imported are the new baseline for any macro that scales them, so drop the old
	// capture and re-run the macros (which re-captures and re-applies the user's exposure).
	if (applied)
	{
		m_macro_base.erase("beam_peak_nits0");
		m_macro_base.erase("hdr_rolloff_max0");
	}
	apply_macros(true);

	if (applied)
	{
		if (m_edr_relative_auto && peak > 0.0f)
			osd_printf_info(
					"BGFX: EDR relative auto-config: nominal peak=%.0f nits (headroom %.2fx of %.0f-nit reference white), beam=%.0f nits (%.2fx reference white); dynamic display ceiling enabled\n",
					peak, peak / paper_white, paper_white, beam, beam / paper_white);
		else if (m_edr_relative_auto)
			osd_printf_info(
					"BGFX: EDR relative auto-config: beam=%.2fx reference white; headroom not resolved yet, dynamic display ceiling enabled\n",
					beam / paper_white);
		else if (m_hdr_display_peak_absolute)
			osd_printf_info(
					"BGFX: HDR auto-config: display=%.0f nits, SDR white=%.1f nits, beam=%.0f nits, rolloff max=%.2f\n",
					peak, paper_white, beam, rmax);
		else
			osd_printf_info("BGFX: HDR relative calibration unavailable\n");
	}
}

void chain_manager::destroy_chains()
{
	for (size_t index = 0; index < m_screen_chains.size(); index++)
	{
		if (m_screen_chains[index] != nullptr)
		{
			delete m_screen_chains[index];
			m_screen_chains[index] = nullptr;
		}
	}
}

void chain_manager::reload_chains()
{
	destroy_chains();
	load_chains();
}

bgfx_chain* chain_manager::screen_chain(uint32_t screen)
{
	if (screen >= m_screen_chains.size())
	{
		return m_screen_chains[m_screen_chains.size() - 1];
	}
	else
	{
		return m_screen_chains[screen];
	}
}

uint32_t chain_manager::process_screen_quad(uint32_t view, uint32_t screen, screen_prim &prim, osd_window& window, bool vector_repeat)
{
	const bool any_targets_rebuilt = m_targets.update_target_sizes(screen, prim.m_tex_width, prim.m_tex_height, TARGET_STYLE_GUEST, m_user_prescale, m_max_prescale_size);
	if (any_targets_rebuilt)
	{
		for (bgfx_chain* chain : m_screen_chains)
		{
			if (chain != nullptr)
			{
				chain->repopulate_targets();
			}
		}
	}

	bgfx_chain* chain = screen_chain(screen);
	const bool repeat_fast_path = vector_repeat && chain->supports_vector_repeat();
	uint32_t used_views = repeat_fast_path ? chain->prepare_vector_repeat(view, screen) : 0;
	used_views += chain->process(prim, view + used_views, screen, m_textures, window, repeat_fast_path);
	return used_views;
}

// inject a GPU-rendered FBO as "screen0" for vector game chain processing.
// Call this once per frame before process_screen_chains() when rendering a vector game.
//
// Design notes:
//   - update_target_sizes() is called BEFORE update_screen_count() so that the
//     "output0" target (if ever created) would have correct dimensions.
//   - m_targets.update_screen_count() is intentionally NOT called here.
//     This leaves "output0" absent from target_manager, so chainentry::setup_view()
//     falls back to BGFX_INVALID_HANDLE (= real backbuffer) for the final pass.
//     No post-chain blit is therefore needed.
//   - prim.m_prim is set to nullptr.  This is safe as long as the chain JSON does not
//     set apply_tint=true on any entry (misc/blit does not).
void chain_manager::inject_vector_screen(bgfx::TextureHandle color_tex,
	uint16_t width, uint16_t height, uint16_t vec_fb_w, uint16_t vec_fb_h,
	uint16_t content_width, uint16_t content_height)
{
	// (1) Set native dims first so any TARGET_STYLE_NATIVE targets get correct sizes.
	m_targets.update_target_sizes(0, width, height, TARGET_STYLE_NATIVE,
		m_user_prescale, m_max_prescale_size);

	// (2) Register FBO color attachment as "screen0" in the chain texture manager.
	m_textures.remove_provider("screen0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("screen0", std::move(prov));

	// (3) Build synthetic screen_prim from window dimensions.
	screen_prim prim;
	prim.m_prim           = nullptr; // safe: chain does not use apply_tint
	prim.m_screen_width   = width;
	prim.m_screen_height  = height;
	prim.m_quad_width     = width;
	prim.m_quad_height    = height;
	prim.m_content_width  = content_width;
	prim.m_content_height = content_height;
	prim.m_tex_width      = float(vec_fb_w);
	prim.m_tex_height     = float(vec_fb_h);
	prim.m_rowpixels      = vec_fb_w;
	prim.m_palette_length = 0;
	prim.m_flags          = 0;

	if (m_screen_prims.empty())
		m_screen_prims.push_back(prim);
	else
		m_screen_prims[0] = prim;

	// (4)+(5) Trigger chain loading on first call (no-op on subsequent frames) and swap a
	// vector-incompatible selection to the first vector chain.
	ensure_vector_screen_slot();

	// (6) Create/recreate the dynamically-sized bloom mip targets when dimensions change.
	// These are sized relative to the window (window/2 .. window/256), so they cannot be declared
	// with fixed sizes in the chain JSON; the vector chains read bloom_lvl0..7 by name.
	if (width != m_vec_win_w || height != m_vec_win_h || vec_fb_w != m_vec_fb_w || vec_fb_h != m_vec_fb_h)
	{
		m_vec_win_w = width;
		m_vec_win_h = height;
		m_vec_fb_w  = vec_fb_w;
		m_vec_fb_h  = vec_fb_h;

		// 8-level mip bloom in the style of MAME's HLSL bloom.fx: lvl0 (window/2) .. lvl7 (window/256).
		const uint16_t bloom_lvl_w[8] = {
			std::max(uint16_t(1), uint16_t(width /   2)),
			std::max(uint16_t(1), uint16_t(width /   4)),
			std::max(uint16_t(1), uint16_t(width /   8)),
			std::max(uint16_t(1), uint16_t(width /  16)),
			std::max(uint16_t(1), uint16_t(width /  32)),
			std::max(uint16_t(1), uint16_t(width /  64)),
			std::max(uint16_t(1), uint16_t(width / 128)),
			std::max(uint16_t(1), uint16_t(width / 256)),
		};
		const uint16_t bloom_lvl_h[8] = {
			std::max(uint16_t(1), uint16_t(height /   2)),
			std::max(uint16_t(1), uint16_t(height /   4)),
			std::max(uint16_t(1), uint16_t(height /   8)),
			std::max(uint16_t(1), uint16_t(height /  16)),
			std::max(uint16_t(1), uint16_t(height /  32)),
			std::max(uint16_t(1), uint16_t(height /  64)),
			std::max(uint16_t(1), uint16_t(height / 128)),
			std::max(uint16_t(1), uint16_t(height / 256)),
		};
		const char* bloom_names[8] = { "bloom_lvl0", "bloom_lvl1", "bloom_lvl2", "bloom_lvl3",
									   "bloom_lvl4", "bloom_lvl5", "bloom_lvl6", "bloom_lvl7" };
		for (int i = 0; i < 8; i++)
		{
			// RGBA16F keeps the bloom seeds linear-capable (no 1.0 clamp, no 8-bit banding in
			// dark glows for the linear HDR chain); the pyramid totals ~1/3 screen so the extra
			// bandwidth is negligible.
			m_targets.create_target(bloom_names[i], bgfx::TextureFormat::RGBA16F,
				bloom_lvl_w[i], bloom_lvl_h[i], 1, 1, TARGET_STYLE_CUSTOM, false, true, 1, 0);
		}
	}
}

// Bootstrap / keep alive the screen-0 chain slot for a vector game WITHOUT injecting the vector
// FBO. The chain-selection slider for a vector game is created by update_screen_count(1), which
// otherwise only happens inside inject_vector_screen on the analytic-engine path - without this,
// launching with a non-engine chain active would never surface the chain selection at all (the
// bootstrap deadlock: the engine only runs when an engine chain is active, but vector chains only
// load when this slot exists). Also swaps a vector-incompatible chain selection to the first
// vector-tagged chain, exactly as inject_vector_screen does (any chain JSON carrying a
// "screen_type": "vector" tag is a candidate).
void chain_manager::ensure_vector_screen_slot()
{
	update_screen_count(1);

	if (!m_screen_chains.empty() && m_current_chain[0] != CHAIN_NONE)
	{
		const size_t cur = size_t(m_current_chain[0]);
		if (cur < m_available_chains.size() && !m_available_chains[cur].m_is_vector)
		{
			// swap to the first vector chain
			for (size_t i = 0; i < m_available_chains.size(); i++)
			{
				if (m_available_chains[i].m_is_vector)
				{
					if (m_screen_chains[0] != nullptr)
					{
						delete m_screen_chains[0];
						m_screen_chains[0] = nullptr;
					}
					m_current_chain[0] = int32_t(i);
					m_chain_names[0] = m_available_chains[i].m_name;
					load_chains();
					break;
				}
			}
		}
	}
}

void chain_manager::inject_vector_glow(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	// Register the analytic-glow FBO colour attachment as "glow0"; a chain pass references it as
	// texture "glow" (chainentryreader treats "glow" as a runtime provider, like "screen").
	m_textures.remove_provider("glow0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("glow0", std::move(prov));
}

void chain_manager::inject_vector_bezel_length(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	// Per-primitive Long contribution generated alongside analytic glow by MRT.
	m_textures.remove_provider("bezel_length0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("bezel_length0", std::move(prov));
}

void chain_manager::inject_vector_flare(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	m_textures.remove_provider("flare0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("flare0", std::move(prov));
}

void chain_manager::inject_vector_overlap(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	// R=sum of bounded overload coverage, G=sum of its square. Kept separate from the
	// visible core so Direct Core Overlap=Uniform Maximum does not erase overlap count.
	m_textures.remove_provider("overlap0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("overlap0", std::move(prov));
}

void chain_manager::inject_vector_optical(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	m_textures.remove_provider("optical0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("optical0", std::move(prov));
}

void chain_manager::inject_vector_np(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	// Register the no-persist FBO colour attachment as "npglow0"; a chain pass references it as
	// texture "npglow" (chainentryreader treats "npglow" as a runtime provider, like "glow").
	m_textures.remove_provider("npglow0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("npglow0", std::move(prov));
}

void chain_manager::inject_vector_dwell(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h)
{
	m_textures.remove_provider("dwell0");
	auto prov = std::make_unique<bgfx_fbo_texture_provider>(color_tex, vec_fb_w, vec_fb_h);
	m_textures.add_provider("dwell0", std::move(prov));
}

uint32_t chain_manager::count_screens(render_primitive* prim)
{
	uint32_t screen_count = 0;
	while (prim != nullptr)
	{
		if (PRIMFLAG_GET_SCREENTEX(prim->flags))
		{
			if (screen_count < m_screen_prims.size())
			{
				m_screen_prims[screen_count] = prim;
			}
			else
			{
				m_screen_prims.push_back(prim);
			}
			screen_count++;
		}
		prim = prim->next();
	}

	if (screen_count > 0)
	{
		update_screen_count(screen_count);
		m_targets.update_screen_count(screen_count, m_user_prescale, m_max_prescale_size);
	}

	if (screen_count < m_screen_prims.size())
	{
		m_screen_prims.resize(screen_count);
	}

	return screen_count;
}

void chain_manager::update_screen_count(uint32_t screen_count)
{
	if (screen_count != m_screen_count)
	{
		m_slider_notifier.set_sliders_dirty();
		m_screen_count = screen_count;

		// Ensure we have one screen chain entry per screen
		while (m_screen_chains.size() < m_screen_count)
		{
			m_screen_chains.push_back(nullptr);

			int32_t chain_index = CHAIN_NONE;
			std::string chain_name;
			get_default_chain_info(chain_name, chain_index);
			m_chain_names.emplace_back(std::move(chain_name));
			m_current_chain.push_back(chain_index);
		}

		// Ensure we have a screen chain selection slider per screen
		while (m_selection_sliders.size() < m_screen_count)
		{
			create_selection_slider(m_selection_sliders.size());
		}

		load_chains();
	}
}

void chain_manager::set_current_chain(uint32_t screen, int32_t chain_index)
{
	if (chain_index < m_available_chains.size() && screen < m_current_chain.size() && screen < m_chain_names.size())
	{
		m_current_chain[screen] = chain_index;
		m_chain_names[screen] = m_available_chains[chain_index].m_name;
	}
}

// The chain-selection slider works in indices into m_compat_chain_indices.
// newval and the return value are compat-list indices; internal m_current_chain[id] holds the absolute index.
int32_t chain_manager::slider_changed(int id, std::string *str, int32_t newval)
{
	if (newval != SLIDER_NOCHANGE)
	{
		// convert the compat-list index to an absolute chain index
		if (newval >= 0 && size_t(newval) < m_compat_chain_indices.size())
		{
			int32_t abs_idx = int32_t(m_compat_chain_indices[newval]);
			set_current_chain(id, abs_idx);

			// Defer the actual destroy/create of the chain's targets to clean frame boundaries via
			// process_pending_reload(); doing it here races in-flight rendering on Metal. Capture the
			// slider settings now (only if the old chains still exist - on a rapid re-switch while a
			// reload is already in flight, keep the earlier capture) so they can be restored after
			// the deferred reload rebuilds the sliders. See m_reload_phase.
			if (m_reload_phase == reload_phase::NONE)
				m_reload_saved_settings = slider_settings();
			m_reload_slider_id = id;
			m_reload_phase = reload_phase::DESTROY;
			// Mark the sliders dirty NOW as well as after the deferred create: while a reload is
			// pending, get_slider_list() returns only the persistent selection sliders (see there),
			// so the menu rebuild this triggers - menu_sliders resets itself right after any slider
			// change - drops every reference into the doomed chains' slider_states BEFORE
			// process_pending_reload() destroys them. Without this, the open menu keeps items
			// pointing at freed slider_states for a frame (occasional crash on adjust/redraw) and,
			// because it never repopulates on its own, keeps displaying the previous chain's
			// parameters (the menu polls for slider-list changes and refreshes once the deferred
			// create publishes the new chain's sliders).
			m_slider_notifier.set_sliders_dirty();
		}
	}

	if (str != nullptr)
	{
		*str = m_available_chains[m_current_chain[id]].m_name;
	}

	// also convert the return value to a compat-list index (the UI displays this index)
	for (size_t i = 0; i < m_compat_chain_indices.size(); i++)
	{
		if (int32_t(m_compat_chain_indices[i]) == m_current_chain[id])
			return int32_t(i);
	}
	return 0;
}

void chain_manager::request_temporal_reset()
{
	// Keep the chain alive so its HDR UI composite remains active on the discontinuity frame.
	// process_screen_chains() clears every current target (both pages for feedback targets) before
	// submitting the selected MVEC frame. A simultaneous selection reload already creates
	// zero-initialized targets, so no additional clear is required.
	if (m_reload_phase == reload_phase::NONE)
		m_temporal_reset_pending = true;
}
void chain_manager::process_pending_reload()
{
	if (m_reload_phase == reload_phase::NONE)
		return;

	if (m_reload_phase == reload_phase::DESTROY)
	{
		// Tear the old chain down this frame and create the replacement only on the NEXT frame: the
		// intervening bgfx::frame() lets the backend (Metal in particular) retire the destroyed
		// textures before their handle slots are recycled by the new targets. See m_reload_phase.
		// This frame renders chain-less (has_applicable_chain() false / null screen_chain are
		// handled everywhere) - a one-frame pass-through blink on switch.
		destroy_chains();
		m_reload_phase = reload_phase::CREATE;
		return;
	}

	m_reload_phase = reload_phase::NONE;
	load_chains();
	restore_slider_settings(m_reload_slider_id, m_reload_saved_settings);
	m_reload_saved_settings.clear();
	m_reload_saved_settings.shrink_to_fit();
	// Mark the slider menu dirty only now, after the new slider_state objects exist, so the UI
	// rebuilds its menu items against the fresh sliders rather than the just-destroyed old ones.
	m_slider_notifier.set_sliders_dirty();
}

void chain_manager::create_selection_slider(uint32_t screen_index)
{
	if (screen_index < m_selection_sliders.size())
	{
		return;
	}

	// The slider value is an index into m_compat_chain_indices.
	// Convert the absolute index (m_current_chain[screen_index]) to its position in the compat list for defval.
	int32_t minval = 0;
	int32_t maxval = m_compat_chain_indices.empty() ? 0 : int32_t(m_compat_chain_indices.size()) - 1;
	int32_t defval = 0;
	for (size_t i = 0; i < m_compat_chain_indices.size(); i++)
	{
		if (int32_t(m_compat_chain_indices[i]) == m_current_chain[screen_index])
		{
			defval = int32_t(i);
			break;
		}
	}
	int32_t incval = 1;

	using namespace std::placeholders;
	auto state = std::make_unique<slider_state>(
			util::string_format("Window %1$u, Screen %2$u Effect", m_window_index, screen_index),
			minval, defval, maxval, incval,
			std::bind(&chain_manager::slider_changed, this, screen_index, _1, _2));

	ui::menu_item item(ui::menu_item_type::SLIDER, state.get());
	item.set_text(state->description);
	m_selection_sliders.emplace_back(item);
	m_core_sliders.emplace_back(std::move(state));
}

uint32_t chain_manager::update_screen_textures(uint32_t view, render_primitive *starting_prim, osd_window& window)
{
	if (!count_screens(starting_prim))
		return 0;

	for (int screen = 0; screen < m_screen_prims.size(); screen++)
	{
		screen_prim &prim = m_screen_prims[screen];
		uint16_t tex_width(prim.m_tex_width);
		uint16_t tex_height(prim.m_tex_height);

		bgfx_texture* texture = screen < m_screen_textures.size() ? m_screen_textures[screen] : nullptr;
		bgfx_texture* palette = screen < m_screen_palettes.size() ? m_screen_palettes[screen] : nullptr;

		const uint32_t src_format = (prim.m_flags & PRIMFLAG_TEXFORMAT_MASK) >> PRIMFLAG_TEXFORMAT_SHIFT;
		const bool needs_conversion = m_converters[src_format] != nullptr;
		const bool needs_adjust = prim.m_prim->texture.palette != nullptr && src_format != TEXFORMAT_PALETTE16;
		const std::string screen_index = std::to_string(screen);
		const std::string source_name = "source" + screen_index;
		const std::string screen_name = "screen" + screen_index;
		const std::string palette_name = "palette" + screen_index;
		const std::string &full_name = (needs_conversion || needs_adjust) ? source_name : screen_name;
		if (texture && (texture->width() != tex_width || texture->height() != tex_height))
		{
			m_textures.remove_provider(full_name);
			m_textures.remove_provider(palette_name);
			texture = nullptr;
			palette = nullptr;
		}

		bgfx::TextureFormat::Enum dst_format = bgfx::TextureFormat::BGRA8;
		uint16_t pitch = prim.m_rowpixels;
		int width_div_factor = 1;
		int width_mul_factor = 1;
		const bgfx::Memory* mem = bgfx_util::mame_texture_data_to_bgfx_texture_data(dst_format, prim.m_flags & PRIMFLAG_TEXFORMAT_MASK,
			prim.m_rowpixels, prim.m_prim->texture.width_margin, tex_height, prim.m_prim->texture.palette, prim.m_prim->texture.base, pitch, width_div_factor, width_mul_factor);

		if (!texture)
		{
			uint32_t flags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
			if (!PRIMFLAG_GET_TEXWRAP(prim.m_flags))
				flags |= BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
			auto newtex = std::make_unique<bgfx_texture>(full_name, dst_format, tex_width, prim.m_prim->texture.width_margin, tex_height, mem, flags, pitch, prim.m_rowpixels, width_div_factor, width_mul_factor);
			texture = newtex.get();
			m_textures.add_provider(full_name, std::move(newtex));

			if (prim.m_prim->texture.palette)
			{
				uint16_t palette_width = uint16_t(std::min(prim.m_palette_length, 256U));
				uint16_t palette_height = uint16_t(std::max((prim.m_palette_length + 255) / 256, 1U));
				m_palette_temp.resize(palette_width * palette_height * 4);
				memcpy(&m_palette_temp[0], prim.m_prim->texture.palette, prim.m_palette_length * 4);
				const bgfx::Memory *palmem = bgfx::copy(&m_palette_temp[0], palette_width * palette_height * 4);
				auto newpal = std::make_unique<bgfx_texture>(palette_name, bgfx::TextureFormat::BGRA8, palette_width, 0, palette_height, palmem, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT, palette_width * 4);
				palette = newpal.get();
				m_textures.add_provider(palette_name, std::move(newpal));
			}

			while (screen >= m_screen_textures.size())
			{
				m_screen_textures.emplace_back(nullptr);
			}
			m_screen_textures[screen] = texture;

			while (screen >= m_screen_palettes.size())
			{
				m_screen_palettes.emplace_back(nullptr);
			}
			if (palette)
			{
				m_screen_palettes[screen] = palette;
			}
		}
		else
		{
			texture->update(mem, pitch, prim.m_prim->texture.width_margin);

			if (prim.m_prim->texture.palette)
			{
				uint16_t palette_width = uint16_t(std::min(prim.m_palette_length, 256U));
				uint16_t palette_height = uint16_t(std::max((prim.m_palette_length + 255) / 256, 1U));
				const uint32_t palette_size = palette_width * palette_height * 4;
				m_palette_temp.resize(palette_size);
				memcpy(&m_palette_temp[0], prim.m_prim->texture.palette, prim.m_palette_length * 4);
				const bgfx::Memory *palmem = bgfx::copy(&m_palette_temp[0], palette_size);

				if (palette)
				{
					palette->update(palmem);
				}
				else
				{
					auto newpal = std::make_unique<bgfx_texture>(palette_name, bgfx::TextureFormat::BGRA8, palette_width, 0, palette_height, palmem, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT, palette_width * 4);
					palette = newpal.get();
					m_textures.add_provider(palette_name, std::move(newpal));
					while (screen >= m_screen_palettes.size())
					{
						m_screen_palettes.emplace_back(nullptr);
					}
					m_screen_palettes[screen] = palette;
				}
			}
		}

		const bool has_tint = (prim.m_prim->color.a != 1.0f) || (prim.m_prim->color.r != 1.0f) || (prim.m_prim->color.g != 1.0f) || (prim.m_prim->color.b != 1.0f);
		bgfx_chain* chain = screen_chain(screen);
		if (chain && needs_adjust && !chain->has_adjuster())
		{
			const bool apply_tint = !needs_conversion && has_tint;
			chain->insert_effect(chain->has_converter() ? 1 : 0, m_adjuster, apply_tint, "XXadjust", needs_conversion ? "screen" : "source", *this);
			chain->set_has_adjuster(true);
		}
		if (chain && needs_conversion && !chain->has_converter())
		{
			chain->insert_effect(0, m_converters[src_format], has_tint, "XXconvert", "source", *this);
			chain->set_has_converter(true);
		}
	}

	return m_screen_prims.size();
}

uint32_t chain_manager::process_screen_chains(uint32_t view, osd_window& window, bool vector_repeat)
{
	// Process each screen as necessary
	uint32_t used_views = 0;
	uint32_t screen_index = 0;
	for (screen_prim &prim : m_screen_prims)
	{
		if (m_current_chain[screen_index] == CHAIN_NONE || screen_chain(screen_index) == nullptr)
		{
			screen_index++;
			continue;
		}

		uint16_t screen_width = prim.m_screen_width;
		uint16_t screen_height = prim.m_screen_height;
		// The synthetic vector prim (m_prim == nullptr, see inject_vector_screen) is already
		// window-oriented: its dimensions come from the window, not from the (possibly rotated)
		// screen. Applying the rotation swap here would disagree with the unswapped dims set in
		// inject_vector_screen step (1), making m_native_dims flip orientation every frame and
		// rebuild every NATIVE render target each frame (massive create/destroy churn: VRAM
		// sawtooth, stale-frame artifacts from recycled target memory, large render stalls).
		if (window.swap_xy() && prim.m_prim != nullptr)
		{
			std::swap(screen_width, screen_height);
		}

		const bool any_targets_rebuilt = m_targets.update_target_sizes(screen_index, screen_width, screen_height, TARGET_STYLE_NATIVE, m_user_prescale, m_max_prescale_size);
		if (any_targets_rebuilt)
		{
			for (bgfx_chain* chain : m_screen_chains)
			{
				if (chain != nullptr)
				{
					chain->repopulate_targets();
				}
			}
		}

		if (m_temporal_reset_pending)
			used_views += screen_chain(screen_index)->clear_targets(view + used_views);

		used_views += process_screen_quad(view + used_views, screen_index, prim, window, vector_repeat);

		screen_index++;
	}

	m_temporal_reset_pending = false;
	bgfx::setViewFrameBuffer(view + used_views, BGFX_INVALID_HANDLE);

	return used_views;
}

bool chain_manager::has_applicable_chain(uint32_t screen)
{
	return (screen < m_screen_count) && (m_current_chain[screen] != CHAIN_NONE) && m_screen_chains[screen];
}

bool chain_manager::needs_sliders()
{
	return (m_screen_count > 0) && (m_available_chains.size() > 1);
}

void chain_manager::restore_slider_settings(int32_t id, std::vector<std::vector<float>>& settings)
{
	if (!needs_sliders())
	{
		return;
	}

	for (size_t index = 0; index < m_screen_chains.size() && index < m_screen_count; index++)
	{
		if (index == id)
		{
			continue;
		}

		bgfx_chain* chain = m_screen_chains[index];
		if (chain == nullptr)
		{
			continue;
		}

		const std::vector<bgfx_slider*> &chain_sliders = chain->sliders();
		for (size_t slider = 0; slider < chain_sliders.size(); slider++)
		{
			chain_sliders[slider]->import(settings[index][slider]);
		}
	}
}

// treat source INI files or more specific as higher priority than CFG
// FIXME: leaky abstraction - this depends on a front-end implementation detail
bool chain_manager::chains_explicitly_specified(const osd_options &options)
{
	return ((OPTION_PRIORITY_NORMAL + 5) <= options.get_entry(OSDOPTION_BGFX_SCREEN_CHAINS)->priority())
			&& *options.bgfx_screen_chains();
}

void chain_manager::load_config(util::xml::data_node const &windownode)
{
	bool const persist = windownode.get_attribute_int("persist", 1) != 0;
	bool const explicit_chains = !persist && chains_explicitly_specified(m_options);

	// if chains weren't explicitly specified, restore the chains from the config file
	if (explicit_chains)
	{
		osd_printf_verbose(
				"BGFX: Ignoring chain selection from window %d configuration due to explicitly specified chains\n",
				m_window_index);
	}
	else
	{
		bool changed = false;
		util::xml::data_node const *screennode = windownode.get_child("screen");
		while (screennode)
		{
			auto const index = screennode->get_attribute_int("index", -1);
			if ((0 <= index) && (m_screen_count > index))
			{
				char const *const chainname = screennode->get_attribute_string("chain", nullptr);
				if (chainname)
				{
					std::string_view const resolved_name = canonical_chain_name(chainname);
					auto const found = std::find_if(
							m_available_chains.begin(),
							m_available_chains.end(),
							[resolved_name] (auto const &avail) { return avail.m_name == resolved_name; });
					if (m_available_chains.end() != found)
					{
						auto const chainnum = found - m_available_chains.begin();
						// Verify screen_type compatibility; ignore an incompatible cfg.
						const bool compat = (size_t(chainnum) == CHAIN_NONE) ||
							(m_available_chains[chainnum].m_is_vector == m_is_vector_game);
						if (!compat)
						{
							osd_printf_warning("BGFX: config chain '%s' not compatible with %s game; ignoring\n",
								chainname, m_is_vector_game ? "vector" : "raster");
						}
						else if (chainnum != m_current_chain[index])
						{
							m_current_chain[index] = chainnum;
							changed = true;
						}
					}
					else if (std::string_view(chainname) != "none" && m_default_chain_index >= 0
						&& m_current_chain[index] != m_default_chain_index)
					{
						osd_printf_warning("BGFX: config chain '%s' is unavailable; using '%s'\n",
							chainname, m_available_chains[m_default_chain_index].m_name.c_str());
						m_current_chain[index] = m_default_chain_index;
						changed = true;
					}
				}
			}

			screennode = screennode->get_next_sibling("screen");
		}

		if (changed)
			reload_chains();
	}

	// now apply slider settings for screens with chains matching config
	util::xml::data_node const *screennode = windownode.get_child("screen");
	while (screennode)
	{
		auto const index = screennode->get_attribute_int("index", -1);
		if ((0 <= index) && (m_screen_count > index) && (m_screen_chains.size() > index))
		{
			bgfx_chain *const chain = m_screen_chains[index];
			char const *const chainname = screennode->get_attribute_string("chain", nullptr);
			if (chain && chainname
				&& (m_available_chains[m_current_chain[index]].m_name == canonical_chain_name(chainname)))
			{
				auto const &sliders = chain->sliders();

				util::xml::data_node const *slidernode = screennode->get_child("slider");
				while (slidernode)
				{
					char const *const slidername = slidernode->get_attribute_string("name", nullptr);
					if (slidername)
					{
						auto const found = std::find_if(
								sliders.begin(),
								sliders.end(),
								[&slidername] (auto const &slider) { return slider->name() == slidername; });
						if (sliders.end() != found)
						{
							bgfx_slider &slider = **found;
							switch (slider.type())
							{
							case bgfx_slider::SLIDER_INT_ENUM:
							case bgfx_slider::SLIDER_INT:
								{
									slider_state const &core = *slider.core_slider();
									int32_t const val = slidernode->get_attribute_int("value", core.defval);
									slider.update(nullptr, std::clamp(val, core.minval, core.maxval));
								}
								break;
							default:
								{
									float const val = slidernode->get_attribute_float("value", slider.default_value());
									slider.import(std::clamp(val, slider.min_value(), slider.max_value()));
								}
							}
						}
					}

					slidernode = slidernode->get_next_sibling("slider");
				}
			}
		}
		screennode = screennode->get_next_sibling("screen");
	}
}

void chain_manager::save_config(util::xml::data_node &parentnode)
{
	if (!needs_sliders())
		return;

	// Do not write a selection that load_config would refuse to read. An explicitly specified chain
	// is a command-line or per-game override of the stored one, and persisting it turns a one-off
	// into the machine's new startup state - running once with -bgfx_screen_chains left starwars
	// on the monochrome chain from then on. The renderer writes the stored selection back verbatim
	// when there was one, so reaching here means there was none to keep.
	if (chains_explicitly_specified(m_options))
	{
		osd_printf_verbose(
				"BGFX: Not saving the chain selection for window %d - it was explicitly specified\n",
				m_window_index);
		return;
	}

	util::xml::data_node *const windownode = parentnode.add_child("window", nullptr);
	windownode->set_attribute_int("index", m_window_index);

	for (size_t index = 0; index < m_screen_chains.size() && index < m_screen_count; index++)
	{
		bgfx_chain *const chain = m_screen_chains[index];
		if (!chain)
			continue;

		util::xml::data_node *const screennode = windownode->add_child("screen", nullptr);
		screennode->set_attribute_int("index", index);
		screennode->set_attribute("chain", m_available_chains[m_current_chain[index]].m_name.c_str());

		for (bgfx_slider *slider : chain->sliders())
		{
			// Hardware-derived HDR defaults are intentionally transient. Do not persist a slider while
			// it still equals the last auto result, otherwise the next launch restores that number from
			// cfg after auto-config and silently turns monitor-dependent calibration into a fixed value.
			// A one-step user edit no longer matches and is saved normally. Relative EDR has no automatic
			// rolloff value (m_hdr_last_auto_rolloff == 0), so its artistic ceiling remains persistable.
			const std::string beam_name = "beam_peak_nits" + std::to_string(index);
			const std::string rmax_name = "hdr_rolloff_max" + std::to_string(index);
			if ((slider->name() == beam_name && m_hdr_last_auto_beam > 0.0f
					&& slider_matches_imported_value(*slider, m_hdr_last_auto_beam))
				|| (slider->name() == rmax_name && m_hdr_last_auto_rolloff > 0.0f
					&& slider_matches_imported_value(*slider, m_hdr_last_auto_rolloff)))
				continue;

			// Same rule for macro-driven sliders: while a target still holds what the macro imported,
			// it is the macro's output and not a user edit, so it must not be written. Nudge it by one
			// step and it stops matching, which is exactly when it becomes worth persisting.
			const auto imported = m_macro_imported.find(slider->name());
			if (imported != m_macro_imported.end() && slider_matches_imported_value(*slider, imported->second))
				continue;

			auto const val = slider->update(nullptr, SLIDER_NOCHANGE);
			if (val == slider->core_slider()->defval)
				continue;

			util::xml::data_node *const slidernode = screennode->add_child("slider", nullptr);
			slidernode->set_attribute("name", slider->name().c_str());
			switch (slider->type())
			{
			case bgfx_slider::SLIDER_INT_ENUM:
			case bgfx_slider::SLIDER_INT:
				slidernode->set_attribute_int("value", val);
				break;
			default:
				slidernode->set_attribute_float("value", slider->value());
			}
		}
	}

	if (!windownode->get_first_child())
		windownode->delete_node();
}

bool chain_manager::inject_entry_uniform(uint32_t screen, const std::string& entry_name,
	const std::string& uniform_name, const float* vals, int count)
{
	if (screen >= m_screen_chains.size() || m_screen_chains[screen] == nullptr)
		return false;
	for (bgfx_chain_entry* entry : m_screen_chains[screen]->entries())
	{
		if (entry->name() == entry_name)
		{
			entry->set_uniform(uniform_name, vals, count);
			return true;
		}
	}
	return false;
}

float chain_manager::slider_value(uint32_t screen, const std::string& name, float default_value)
{
	if (screen >= m_screen_chains.size() || m_screen_chains[screen] == nullptr)
		return default_value;
	// slider_reader registers a float slider under name + "0", so match the "0"-suffixed name.
	const std::string suffixed = name + "0";
	for (bgfx_slider* slider : m_screen_chains[screen]->sliders())
	{
		if (slider->name() == suffixed)
			return slider->value();
	}
	return default_value;
}

float chain_manager::slider_value_indexed(uint32_t screen, const std::string& name, int index, float default_value)
{
	if (screen >= m_screen_chains.size() || m_screen_chains[screen] == nullptr)
		return default_value;
	const std::string suffixed = name + std::to_string(index);
	for (bgfx_slider* slider : m_screen_chains[screen]->sliders())
	{
		if (slider->name() == suffixed)
			return slider->value();
	}
	return default_value;
}

std::vector<std::vector<float>> chain_manager::slider_settings()
{
	std::vector<std::vector<float>> curr;

	if (!needs_sliders())
	{
		return curr;
	}

	for (size_t index = 0; index < m_screen_chains.size() && index < m_screen_count; index++)
	{
		curr.push_back(std::vector<float>());

		bgfx_chain* chain = m_screen_chains[index];
		if (chain == nullptr)
		{
			continue;
		}

		const std::vector<bgfx_slider*> &chain_sliders = chain->sliders();
		for (bgfx_slider* slider : chain_sliders)
		{
			curr[index].push_back(slider->value());
		}
	}

	return curr;
}

std::vector<ui::menu_item> chain_manager::get_slider_list()
{
	std::vector<ui::menu_item> sliders;

	if (!needs_sliders())
	{
		return sliders;
	}

	for (size_t index = 0; index < m_screen_chains.size() && index < m_screen_count; index++)
	{
		bgfx_chain* chain = m_screen_chains[index];
		sliders.push_back(m_selection_sliders[index]);

		// While a deferred chain reload is pending the current chains (and their slider_states)
		// are about to be destroyed - publish only the persistent selection sliders so a menu
		// rebuilt in this window holds no references into the doomed chains. The reload's CREATE
		// phase marks the sliders dirty again once the replacement sliders exist.
		if (chain == nullptr || m_reload_phase != reload_phase::NONE)
		{
			continue;
		}

		const std::vector<bgfx_chain_entry*> &chain_entries = chain->entries();
		for (bgfx_chain_entry* entry : chain_entries)
		{
			const std::vector<bgfx_input_pair*> &entry_inputs = entry->inputs();
			for (bgfx_input_pair* input : entry_inputs)
			{
				std::vector<ui::menu_item> input_sliders = input->get_slider_list();
				for (ui::menu_item &slider : input_sliders)
				{
					sliders.emplace_back(slider);
				}
			}
		}

		const std::vector<bgfx_slider*> &chain_sliders = chain->sliders();
		// Advanced toggle: while it is off, sliders tagged "advanced" are withheld from the menu. Their
		// values keep applying and keep being saved - this only decides what the user has to scroll
		// past. A chain without the toggle shows everything, exactly as before.
		bool show_advanced = true;
		for (bgfx_slider* slider : chain_sliders)
		{
			if (slider->name() == "advanced_sliders0")
			{
				show_advanced = slider->value() > 0.5f;
				break;
			}
		}
		size_t published = 0;
		for (bgfx_slider* slider : chain_sliders)
		{
			if (slider->advanced() && !show_advanced)
				continue;
			published++;
			slider_state *const core_slider = slider->core_slider();

			ui::menu_item item(ui::menu_item_type::SLIDER, core_slider);
			item.set_text(core_slider->description);
			m_selection_sliders.emplace_back(item);

			sliders.emplace_back(std::move(item));
		}

		osd_printf_verbose("BGFX: slider menu for screen %u: %u of %u published (advanced %s)\n",
			unsigned(index), unsigned(published), unsigned(chain_sliders.size()),
			show_advanced ? "on" : "off");

		if (published > 0)
		{
			ui::menu_item item(ui::menu_item_type::SEPARATOR);
			item.set_text(MENU_SEPARATOR_ITEM);

			sliders.emplace_back(std::move(item));
		}
	}

	return sliders;
}
