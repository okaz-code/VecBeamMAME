// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  chainmanager.h - BGFX shader chain manager
//
//  Provides loading for BGFX shader effect chains, defined
//  by chain.h and read by chainreader.h
//
//============================================================

#ifndef MAME_RENDER_BGFX_CHAINMANAGER_H
#define MAME_RENDER_BGFX_CHAINMANAGER_H

#pragma once

#include "effectmanager.h"
#include "targetmanager.h"
#include "texturemanager.h"

#include "util/utilfwd.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


class running_machine;
class osd_window;
struct slider_state;
class slider_dirty_notifier;
class render_primitive;

namespace ui { class menu_item; }

class bgfx_chain;
class bgfx_slider;

class chain_manager
{
public:
	class screen_prim
	{
	public:
		screen_prim() = default;
		screen_prim(render_primitive *prim);

		render_primitive *m_prim = nullptr;
		uint16_t m_screen_width = 0;
		uint16_t m_screen_height = 0;
		uint16_t m_quad_width = 0;
		uint16_t m_quad_height = 0;
		uint16_t m_content_width = 0;
		uint16_t m_content_height = 0;
		float m_tex_width = 0.0f;
		float m_tex_height = 0.0f;
		int m_rowpixels = 0;
		uint32_t m_palette_length = 0;
		uint32_t m_flags = 0;
	};

	chain_manager(running_machine& machine, const osd_options& options, texture_manager& textures, target_manager& targets, effect_manager& effects, uint32_t window_index,
		slider_dirty_notifier& slider_notifier, uint16_t user_prescale, uint16_t max_prescale_size);
	~chain_manager();

	uint32_t update_screen_textures(uint32_t view, render_primitive *starting_prim, osd_window& window);
	uint32_t process_screen_chains(uint32_t view, osd_window& window, bool vector_repeat = false);

	// inject a GPU-rendered FBO as "screen0" for vector game chain processing.
	// Must be called before process_screen_chains() each frame.
	// vec_fb_w/h are the supersample FBO dimensions (typically 2x window).
	void inject_vector_screen(bgfx::TextureHandle color_tex, uint16_t width, uint16_t height,
		uint16_t vec_fb_w, uint16_t vec_fb_h, uint16_t content_width, uint16_t content_height);

	// Bootstrap / keep alive the screen-0 chain slot for a vector game without injecting the
	// vector FBO: keeps the chain-selection slider available while the active chain does not opt
	// into the analytic vector engine (selecting an engine chain then re-engages the FBO path).
	void ensure_vector_screen_slot();

	// Register a second GPU-rendered FBO as "glow0" - the analytic glow drawn separately so a chain
	// can composite it AFTER the shadow mask (scattered light is not masked). Must be called before
	// process_screen_chains() each frame when analytic glow is active.
	void inject_vector_glow(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);
	void inject_vector_bezel_length(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);
	void inject_vector_flare(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);
	void inject_vector_overlap(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);

	// Register the explicit optical-effects FBO as "optical0". Halation rims/fill and
	// starburst rays stay separate from ordinary glow so tail shaping cannot suppress them.
	void inject_vector_optical(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);

	// Register the no-persist FBO as "npglow0" - line caps / short-dwell junction dots drawn
	// separately so a chain can add them back AFTER the phosphor pool (bright, no afterimage) without
	// feeding them into the glow cascade. Must be called before process_screen_chains() each frame.
	void inject_vector_np(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);

	// Register the core FBO's second attachment as "dwell0" - per-pixel terminus dwell excess, used
	// by the phosphor passes to raise masked_core_peak only where the beam actually stopped.
	void inject_vector_dwell(bgfx::TextureHandle color_tex, uint16_t vec_fb_w, uint16_t vec_fb_h);

	// Getters
	running_machine& machine() const { return m_machine; }
	const osd_options& options() const { return m_options; }
	texture_manager& textures() const { return m_textures; }
	target_manager& targets() const { return m_targets; }
	effect_manager& effects() const { return m_effects; }
	slider_dirty_notifier& slider_notifier() const { return m_slider_notifier; }
	uint32_t window_index() const { return m_window_index; }
	uint32_t screen_count() const { return m_screen_count; }
	bgfx_chain* screen_chain(uint32_t screen);
	// Look up the current value of a named float slider in a screen's active chain.
	// Returns default_value when the chain or slider is absent, so a renderer can read
	// optional per-chain parameters by name without the chain having to define them.
	float slider_value(uint32_t screen, const std::string& name, float default_value);
	// Like slider_value, but for one component of a multi-component slider (vec2/vec4/color), which
	// sliderreader.cpp registers as separate named sliders "name0", "name1", ... (NOT reachable via
	// slider_value, which always appends "0" to whatever name it's given). Returns default_value when
	// that component isn't registered (e.g. a plain float slider has no "name1") - callers can pass a
	// sensible per-chain fallback so older chains with a scalar version of a slider degrade gracefully.
	float slider_value_indexed(uint32_t screen, const std::string& name, int index, float default_value);
	// Override a named uniform on a named pass of a screen's active chain (for per-frame CPU values).
	// Returns false when the chain or pass is absent. No-op if the pass does not use the uniform.
	bool inject_entry_uniform(uint32_t screen, const std::string& entry_name,
		const std::string& uniform_name, const float* vals, int count);
	std::unique_ptr<bgfx_chain> load_chain(std::string name, uint32_t screen_index);
	bool has_applicable_chain(uint32_t screen);
	std::vector<ui::menu_item> get_slider_list();
	std::vector<std::vector<float>> slider_settings();

	// Setters
	void restore_slider_settings(int32_t id, std::vector<std::vector<float>>& settings);

	void load_config(util::xml::data_node const &screennode);
	void save_config(util::xml::data_node &parentnode);

	// True when -bgfx_screen_chains was set at source.ini priority or above, i.e. per-source,
	// per-game or on the command line rather than as a global preference in mame.ini. The stored
	// selection is neither read nor written in that case; see load_config and save_config.
	// Static because the renderer has to ask before the chain manager exists.
	static bool chains_explicitly_specified(const osd_options &options);

	// Apply a chain-selection change requested via slider_changed(). The actual reload_chains()
	// (which destroys and recreates all of the chain's bgfx targets) is deferred to here so it runs
	// from the renderer at a clean frame boundary rather than mid-frame inside the slider callback -
	// see the member comment on m_reload_pending. No-op when nothing is pending.
	void process_pending_reload();
	// Clear temporal FBO history without destroying the active chain.
	void request_temporal_reset();

	// HDR auto-config calibration. Absolute-peak platforms derive both values from nits. Relative
	// macOS EDR auto uses a stable artistic beam and leaves the hardware ceiling to a dynamic present
	// uniform instead of freezing a transient headroom ratio into the sliders.
	void set_hdr_display_peak(float nits, bool absolute = true, bool edr_relative_auto = false)
	{
		m_hdr_display_peak = nits;
		m_hdr_display_peak_absolute = absolute;
		m_edr_relative_auto = edr_relative_auto;
	}
	void set_hdr_paper_white(float nits) { m_hdr_paper_white = nits; }
	// MACRO sliders: import the values they derive into their target sliders. force = the user moved a
	// macro (or the chain was just loaded), so the targets are overwritten; otherwise only macros whose
	// value actually changed are applied. Called once per frame from the renderer, which is how a menu
	// edit is noticed - bgfx_slider has no change callback.
	void apply_macros(bool force = false);
	// Update hardware-derived defaults after the host window crosses to another monitor. This is
	// intentionally separate from the initial setter because the first application occurs as part of
	// load_chains(), while a live display change must update the already-loaded sliders immediately.
	void refresh_hdr_display(float nits, bool absolute, bool edr_relative_auto, float paper_white)
	{
		set_hdr_display_peak(nits, absolute, edr_relative_auto);
		set_hdr_paper_white(paper_white);
		m_hdr_live_refresh = true;
		apply_hdr_auto();
		m_hdr_live_refresh = false;
	}

private:
	class chain_desc
	{
	public:
		chain_desc(const chain_desc &) = default;
		chain_desc(chain_desc &&) = default;
		chain_desc &operator=(const chain_desc &) = default;
		chain_desc &operator=(chain_desc &&) = default;

		chain_desc(std::string &&name, std::string &&path, bool is_vector = false)
			: m_name(std::move(name))
			, m_path(std::move(path))
			, m_is_vector(is_vector)
		{
		}

		std::string m_name;
		std::string m_path;
		bool        m_is_vector = false;  // from the chain JSON "screen_type": "vector" tag
	};

	void load_chains();
	void destroy_chains();
	void reload_chains();

	// Derive and import HDR slider values from m_hdr_display_peak (no-op when 0 or bgfx_hdr off).
	// Called at the end of load_chains() so the values act as computed defaults: user cfg (restored
	// later) and live slider edits (restored across reloads) both take precedence.
	void apply_hdr_auto();


	// Rebuild m_compat_chain_indices according to m_is_vector_game.
	void rebuild_compat_chain_indices();
	// Detect the game and vector-monitor type from the machine (called once in the constructor).
	void detect_vector_game();
	int32_t find_chain_index(std::string_view name) const;
	int32_t find_vector_fallback_index(bool include_profile_chain) const;
	std::string_view preferred_vector_chain() const;
	static std::string_view canonical_chain_name(std::string_view name);

	void init_texture_converters();

	void get_default_chain_info(std::string &out_chain_name, int32_t &out_chain_index);
	void refresh_available_chains();
	void destroy_unloaded_chains();
	void find_available_chains(std::string_view root, std::string_view path);
	void parse_chain_selections(std::string_view chain_str);
	std::vector<std::string_view> split_option_string(std::string_view chain_str) const;

	void update_screen_count(uint32_t screen_count);

	void set_current_chain(uint32_t screen, int32_t chain_index);
	int32_t slider_changed(int id, std::string *str, int32_t newval);
	void create_selection_slider(uint32_t screen_index);
	bool needs_sliders();

	uint32_t count_screens(render_primitive* prim);
	uint32_t process_screen_quad(uint32_t view, uint32_t screen, screen_prim &prim, osd_window& window, bool vector_repeat);

	running_machine&            m_machine;
	const osd_options&          m_options;
	texture_manager&            m_textures;
	target_manager&             m_targets;
	effect_manager&             m_effects;
	uint32_t                    m_window_index;
	uint16_t                    m_user_prescale;
	uint16_t                    m_max_prescale_size;
	slider_dirty_notifier&      m_slider_notifier;
	uint32_t                    m_screen_count;
	int32_t                     m_default_chain_index;
	std::vector<chain_desc>     m_available_chains;
	std::vector<bgfx_chain*>    m_screen_chains;
	std::vector<std::string>    m_chain_names;
	std::vector<ui::menu_item>  m_selection_sliders;
	std::vector<std::unique_ptr<slider_state>> m_core_sliders;
	std::vector<int32_t>        m_current_chain;
	std::vector<bgfx_texture*>  m_screen_textures;
	std::vector<bgfx_texture*>  m_screen_palettes;
	std::vector<bgfx_effect*>   m_converters;
	bgfx_effect *               m_adjuster;
	std::vector<screen_prim>    m_screen_prims;
	std::vector<uint8_t>        m_palette_temp;

	static inline constexpr uint32_t CHAIN_NONE = 0;
	enum class vector_monitor_type : uint8_t { UNKNOWN, COLOR, MONOCHROME, VECTREX };
	vector_monitor_type m_vector_monitor_type = vector_monitor_type::UNKNOWN;

	// Window/FBO dimensions from the last inject_vector_screen() call; used to recreate the
	// dynamically-sized bloom mip targets only when the size changes.
	uint16_t m_vec_win_w = 0;
	uint16_t m_vec_win_h = 0;
	uint16_t m_vec_fb_w  = 0;
	uint16_t m_vec_fb_h  = 0;

	// Deferred chain reload. A chain-selection slider change (slider_changed) only records the request
	// here; the destroy and recreate of the chain's bgfx targets are carried out later by
	// process_pending_reload(), called from the renderer before it starts a frame. Performing the
	// reload directly in the slider callback tears down and rebuilds the render targets while the
	// previous frame's GPU work may still be in flight, which races on the Metal backend and corrupts
	// the display (individual vector lines fly to bounded-random positions, occasional blowout bloom)
	// until the app is restarted. The reload is additionally split into TWO frames - destroy on one,
	// create on the next - because destroy+create within the same frame still corrupted intermittently
	// on Metal (freed handle slots are recycled by the new targets while the in-flight frame's command
	// buffers still reference the old resources); the intervening bgfx::frame() lets the backend
	// retire the old textures first, at the cost of one chain-less (pass-through) frame on switch.
	// D3D/GL were tolerant either way but share the cleaner ordering.
	enum class reload_phase { NONE, DESTROY, CREATE };
	reload_phase                    m_reload_phase = reload_phase::NONE;
	int32_t                         m_reload_slider_id = 0;
	std::vector<std::vector<float>> m_reload_saved_settings;
	bool                            m_temporal_reset_pending = false;

	// HDR auto-config calibration peak; absolute nits when known. Relative EDR auto is ratio-based.
	float                           m_hdr_display_peak = 0.0f;
	float                           m_hdr_paper_white = 200.0f;
	bool                            m_hdr_display_peak_absolute = true;
	bool                            m_edr_relative_auto = false;
	float                           m_hdr_last_auto_beam = 0.0f;
	float                           m_hdr_last_auto_rolloff = 0.0f;
	bool                            m_hdr_live_refresh = false;
	// Last macro value applied, and the value last imported into each target. The latter is what keeps
	// macro-derived numbers OUT of the cfg (see save_config): persisting them would freeze the macro's
	// output as a fixed user edit on the next launch.
	std::map<std::string, float>    m_macro_last;
	std::map<std::string, float>    m_macro_imported;
	// Baseline a macro scales from: the value a target held BEFORE any macro touched it. That is the
	// chain default for most sliders, but beam_peak_nits is auto-derived from the display first, and
	// scaling its JSON default instead would throw the HDR auto-config away.
	std::map<std::string, float>    m_macro_base;

	// Game type (initialized in the constructor)
	bool m_is_vector_game = false;
	// Absolute indices into m_available_chains that are compatible with the current game type,
	// used to filter the UI selection slider's choices by screen_type. CHAIN_NONE (0) is always included.
	std::vector<size_t> m_compat_chain_indices;
};

#endif // MAME_RENDER_BGFX_CHAINMANAGER_H
