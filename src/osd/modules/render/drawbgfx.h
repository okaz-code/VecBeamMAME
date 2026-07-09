// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
#ifndef MAME_RENDER_DRAWBGFX_H
#define MAME_RENDER_DRAWBGFX_H

#pragma once

#include "binpacker.h"
#include "bgfx/chain.h"
#include "bgfx/chainmanager.h"
#include "bgfx/slider.h"
#include "bgfx/vertex.h"
#include "sliderdirtynotifier.h"

#include "modules/osdwindow.h"

#include "notifier.h"

#include <bgfx/bgfx.h>

#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>


class texture_manager;
class target_manager;
class shader_manager;
class effect_manager;
class bgfx_texture;
class bgfx_effect;
class bgfx_target;
class bgfx_chain;
class bgfx_view;
class osd_options;
class avi_write;

/* renderer_bgfx is the information about BGFX for the current screen */
class renderer_bgfx : public osd_renderer, public slider_dirty_notifier
{
public:
	class parent_module;

	renderer_bgfx(osd_window &window, parent_module &parent_module);
	virtual ~renderer_bgfx();

	virtual int create() override;
	virtual int draw(const int update) override;

	virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override;
	virtual std::vector<ui::menu_item> get_slider_list() override;
	virtual void set_sliders_dirty() override;

#ifdef OSD_SDL
	virtual int xy_to_render_target(const int x, const int y, int *xt, int *yt) override;
#endif

	virtual void save() override { }
	virtual void record() override;
	virtual void toggle_fsfx() override { }

	uint32_t get_window_width(uint32_t index) const;
	uint32_t get_window_height(uint32_t index) const;

	virtual render_primitive_list *get_primitives() override;

	static char const *const WINDOW_PREFIX;

private:
	enum buffer_status
	{
		BUFFER_PRE_FLUSH,
		BUFFER_FLUSH,
		BUFFER_SCREEN,
		BUFFER_EMPTY,
		BUFFER_DONE
	};

	class parent_module_holder
	{
	public:
		parent_module_holder(parent_module &parent);
		~parent_module_holder();
		parent_module &operator()() const { return m_parent; }
	private:
		parent_module &m_parent;
	};

	void vertex(ScreenVertex* vertex, float x, float y, float z, uint32_t rgba, float u, float v);
	void render_avi_quad();
	void update_recording();

	bool update_dimensions();

	void setup_ortho_view();

	void allocate_buffer(render_primitive *prim, uint32_t blend, bgfx::TransientVertexBuffer *buffer);
	buffer_status buffer_primitives(bool atlas_valid, render_primitive** prim, bgfx::TransientVertexBuffer* buffer, int32_t screen, int window_index);

	void render_textured_quad(render_primitive* prim, bgfx::TransientVertexBuffer* buffer, int window_index);
	void render_post_screen_quad(int view, render_primitive* prim, bgfx::TransientVertexBuffer* buffer, int32_t screen, int window_index);

	void put_packed_quad(render_primitive *prim, uint32_t hash, ScreenVertex* vertex);
	void put_packed_line(render_primitive *prim, ScreenVertex* vertex);
	void put_polygon(const float* coords, uint32_t num_coords, float r, uint32_t rgba, ScreenVertex* vertex);
	void put_line(float x0, float y0, float x1, float y1, float r, uint32_t rgba, ScreenVertex* vertex, float fth = 1.0f);

	// Solid line without AA (for FBO-based vector drawing).
	// Draws a line as a single solid quad from 6 vertices. Unlike put_packed_line it has no
	// transparent edge, so it keeps uniform intensity regardless of line width and angle.
	void put_solid_line(render_primitive *prim, ScreenVertex* vertex);

	// Unified beam-energy model for sources that do NOT supply beam_energy (DVG / AVG / Cinematronics):
	// derive it renderer-side from the per-segment timestamps, in the same convention as the Vectrex
	// driver (0..1 = normal display range, > 1 = overdrive from slow sweeps / dwelling dots). Returns a
	// value on the display-intensity scale (0..1 = normal, > 1 = overdrive). Returns the plain display
	// intensity when the model is off or timestamps are unavailable. stroke_px_per_ms (>= 0) replaces
	// the per-segment sweep speed with the whole stroke's aggregate (see m_stroke_speed).
	float generic_beam_energy(render_primitive *prim, float seg_len, bool as_point, float screen_ref, float stroke_px_per_ms = -1.0f);

	// Port of the Vectrex driver's object_boost() (vectrex_v.cpp): a smooth per-object-type lift of
	// beam_energy, driven by display intensity - a bullet/explosion/star (typically drawn at higher
	// intensity than an enemy/ship) sails past energy_obj_knee and multiplies up to energy_obj_max,
	// with an extra energy_obj_star factor for point-classified (parked-dot) primitives. Model-derived
	// energy only; a device that supplies its own beam_energy already includes any such lift itself.
	// energy_obj_lift <= 0 = off (multiplier 1, unaffected). The caller applies this to POINTS ONLY
	// (see put_analytic_line) so it does not amplify fast multiplexed line content into a visible beat.
	float energy_object_lift(float intensity01, bool as_point) const;

	// DAC / integrator position noise: time-coherent, position-keyed 2D endpoint offset (px). See the
	// definition; shared vertices / coincident dot endpoints get the same offset so the path stays joined.
	void beam_noise_offset(float x, float y, float &ox, float &oy) const;

	void set_bgfx_state(uint32_t blend);

	static uint32_t u32Color(uint32_t r, uint32_t g, uint32_t b, uint32_t a);

	bool check_for_dirty_atlas();
	bool update_atlas();
	void process_atlas_packs(std::vector<std::vector<rectangle_packer::packed_rectangle>>& packed);
	uint32_t get_texture_hash(render_primitive *prim);

	void load_config(util::xml::data_node const &parentnode);
	void save_config(util::xml::data_node &parentnode);

	parent_module_holder m_module; // keep this where it will be destructed last

	bgfx_target *m_framebuffer;
	bgfx_texture *m_texture_cache;

	// Original display_mode
	osd_dim m_dimensions;

	std::unique_ptr<texture_manager> m_textures;
	std::unique_ptr<target_manager> m_targets;
	std::unique_ptr<shader_manager> m_shaders;
	std::unique_ptr<effect_manager> m_effects;
	std::unique_ptr<chain_manager> m_chains;

	bgfx_effect *m_gui_effect[4];
	bgfx_effect *m_screen_effect[4];
	std::vector<uint32_t> m_seen_views;

	// FBO for vector drawing in the BGFX-sample style (not routed through chain_manager).
	// m_vec_supersample is the FBO supersampling factor, from -bgfx_vec_supersample (clamped 1-2).
	uint16_t m_vec_supersample = 2;
	bgfx::FrameBufferHandle m_vec_fb = BGFX_INVALID_HANDLE;
	uint16_t m_vec_fb_w = 0;
	uint16_t m_vec_fb_h = 0;
	bool m_vectors_in_fbo = false;  // whether vector LINEs were drawn into the FBO this frame
	// Chain-driven opt-in: true while the active screen-0 chain declares "vector_engine":
	// "analytic" (see bgfx_chain::vector_engine). While false the vector FBOs are released and
	// vector LINE primitives take the stock buffer_primitives path untouched.
	bool m_vec_engine_active = false;
	// Emulated time (ms) at this present, cached once per frame for the Energy Jitter time axis.
	double m_vec_time_ms = 0.0;
	// Renderer energy-model aids, rebuilt by a per-frame pre-pass when active (see draw()):
	// - m_stroke_speed: per-primitive stroke-aggregate beam speed (px/ms) from cap_flags-delimited
	//   stroke runs (Vectrex RAMP strokes; sources without cap_flags never populate it), smoothing
	//   per-segment quantization noise along curves.
	// - m_dwell_scale: per-primitive energy scale (< 1) capping the additive pile-up of consecutive
	//   dots parked at one spot (the Vectrex driver model's "Dwell accum limit" equivalent).
	std::unordered_map<const render_primitive*, float> m_stroke_speed;
	std::unordered_map<const render_primitive*, float> m_dwell_scale;

public:
	// Per-frame cache of every chain slider the per-vector hot paths read. slider_value() is a
	// string concatenation plus a linear scan over every chain slider; put_analytic_line reads
	// dozens of sliders per vector, so on a busy scene that was >100k lookups per frame. The
	// cache is refreshed once per draw() (~80 lookups), and the hot paths read plain floats.
	// Field defaults mirror the read sites' fallback values, so frames rendered before the
	// first refresh (or without a chain) behave exactly like the old chain-less reads.
	struct vec_slider_cache
	{
		float analytic_glow = 0.0f;
		float analytic_glow_width = 8.0f;
		float beam_noise = 0.0f;   // DAC/integrator position noise amplitude (px); 0 = off
		float beam_width_max = 1.5f;
		float beam_width_min = 1.0f;
		float beam_width_over_scale = -1.0f;
		float beam_width_overmax = 4.0f;
		float bright_curve = 1.0f;
		float bright_normal_cap = 1.0f;
		float bright_sigmoid = 0.0f;
		float bright_sigmoid_center = 0.5f;
		float bright_threshold = 0.0f;
		float core_flat = 0.0f;
		float deflection_damping = 0.5f;
		float deflection_dynamics = 0.0f;
		float deflection_settle = 5.0f;
		float dot_no_persist_dwell = 0.0f;
		float edge_defocus = 0.0f;
		float edge_defocus_curve = 2.0f;
		float energy_curve = 1.0f;
		float energy_dot_curve = 1.6f;
		float energy_dot_max = 3.2f;
		float energy_dot_ref = 30.0f;
		float energy_dwell_cap = 16.0f;   // 16 = no cap (chains without the slider)
		float energy_infl = 0.6f;
		float energy_jitter = 0.0f;
		float energy_jitter_base = 0.0f;   // always-on wobble floor for normal vectors (analog noise)
		float energy_jitter_hz = 15.0f;
		float energy_jitter_onset = 0.8f;
		float energy_jitter_ramp = 0.5f;
		float energy_line_max = 4.0f;
		float energy_model = 0.0f;
		float energy_obj_knee = 0.75f;
		float energy_obj_lift = 0.0f;     // 0 = off (chains without the slider unchanged)
		float energy_obj_max = 3.0f;
		float energy_obj_sharp = 2.0f;
		float energy_obj_star = 1.5f;
		float energy_speed_norm = 0.8f;
		float energy_stroke_agg = 1.0f;
		float glow_curve = 1.0f;
		float glow_narrow = 0.0f;
		float glow_threshold = 0.0f;
		float hv_droop = 0.0f;
		float intensity_overdrive = 0.0f;
		float intensity_overdrive_curve = 2.0f;
		float z_rise_tau = 0.0f;   // Z rise-time (us); 0 = off. Dims brief-dwell dots (see put_analytic_line).
		float line_cap_brightness = 1.0f;
		float line_cap_intensity_curve = 0.0f;
		float line_cap_min_size = 0.0f;
		float line_cap_size = 2.0f;         // = LINE_CAP_SIZE_PX
		float line_cap_width = 1.5f;
		float line_point_threshold = 2.0f;  // = LINE_POINT_THRESHOLD
		float linear_color = 0.0f;
		float overdrive_core = 0.0f;
		float overdrive_sat_curve = 1.0f;
		float overload_bloom = 0.0f;
		float overload_dot_gain = 1.0f;
		float overload_gain = 0.0f;
		float overload_gain_center = 0.5f;
		float overload_glow_gain = 0.0f;
		float overload_glow_width = 40.0f;
		float overload_max = 0.0f;
		float overload_ramp = 0.0f;
		float overload_threshold = 1.0f;
		float phosphor_overdrive = 0.0f;
		float point_width_scale = 1.0f;
		float ray_angle = 15.0f;
		float ray_count_rand = 0.0f;    // time-varying random suppression of individual rays (0 = all present)
		float ray_gain = 0.0f;
		float ray_length = 60.0f;
		float ray_length_rand = 0.0f;   // time-varying per-ray length wobble (0 = static pattern)
		float ray_var = 0.6f;
		float ray_width = 1.2f;
		float ring_fill = 0.0f;
		float ring_gain = 0.0f;
		float ring_min_dwell = 0.0f;
		float ring_over_gain = 0.0f;
		float ring_radius = 24.0f;
		float ring_threshold = 0.0f;
		float ring_width = 3.0f;
		float vector_linearity_x = 1.0f;
		float vector_linearity_y = 1.0f;
		float width_curve = 1.0f;
		float width_knee = 0.3f;
		float width_over_curve = 1.0f;
		float width_sigmoid = 0.0f;
		float width_sigmoid_center = 0.5f;
		// overdrive_knee is a vec2 (knee, ceiling); the ceiling is pre-guarded to knee + eps in
		// the refresh so chains still carrying a plain float knee keep the hard-step behaviour.
		float overdrive_knee = 0.6f;
		float overdrive_ceil = 0.6001f;
		float overdrive_color[3] = { 1.0f, 1.0f, 1.0f };
	};

private:
	vec_slider_cache m_vs;
	void refresh_vec_slider_cache();
	void rebuild_vec_slider_map();
	// name->pointer map for the cache refresh, rebuilt when the screen-0 chain changes
	std::vector<std::pair<float vec_slider_cache::*, bgfx_slider*>> m_vs_map;
	bgfx_chain *m_vs_src_chain = nullptr;
	bgfx_slider *m_vs_knee0 = nullptr;
	bgfx_slider *m_vs_knee1 = nullptr;
	bgfx_slider *m_vs_ovcol[3] = { nullptr, nullptr, nullptr };
	// Analytic glow FBO: the wide-gaussian glow is drawn here, separate from the core m_vec_fb,
	// so a chain pass can add it AFTER the shadow mask (scattered light is unmasked). Colour-only.
	// Injected to the chain as "glow0" when analytic glow is active. Sized m_vec_fb * the active
	// chain's glow_fbo_scale slider (1.0 when the chain has no such slider): glow content is smooth
	// wide gaussians evaluated analytically from interpolated line-local varyings (no gl_FragCoord),
	// so a reduced raster just samples the same function at lower density - the -lite variant chain
	// sets 0.5 to quarter the fill cost of the biggest fill-rate consumer (200px oglow footprints).
	bgfx::FrameBufferHandle m_vec_glow_fb = BGFX_INVALID_HANDLE;
	uint16_t m_vec_glow_fb_w = 0;
	uint16_t m_vec_glow_fb_h = 0;
	// No-persist FBO: line end caps and short-dwell junction dots are drawn here (bypassing the
	// phosphor pool) so a chain pass can add them back AFTER the pool - bright while drawn, no
	// afterimage - without ever feeding them into the narrow/wide glow cascade (which m_vec_glow_fb
	// shares). Same size/format as m_vec_glow_fb; colour-only. Exposed to the chain as "npglow".
	bgfx::FrameBufferHandle m_vec_np_fb = BGFX_INVALID_HANDLE;

	// Analytic-AA vector line effect (fs_vector_line). Draws vector LINEs into m_vec_fb.
	// The subsequent post-processing is handled by the chain (JSON).
	bgfx_effect* m_line_effect = nullptr;
	// -bgfx_vec_line_shader analytic: gaussian line integral renderer (erf closed form,
	// 18 verts/line on AnalyticLineVertex: body quad + two gaussian end-cap dots).
	bool m_line_analytic = false;
	void put_analytic_line(render_primitive *prim, AnalyticLineVertex *vertex, AnalyticLineVertex *glow_vertex = nullptr, AnalyticLineVertex *np_vertex = nullptr, AnalyticLineVertex *ray_vertex = nullptr, float start_cap = 1.0f, float end_cap = 1.0f, float stroke_px_per_ms = -1.0f, float dwell_scale = 1.0f);

	// Deflection-amplifier dynamics: the AVG X/Y deflection amps are second-order
	// systems, so the actual beam lags the commanded ramp and overshoots at direction changes (corner
	// "hooks", curved starts after a jump). When enabled, the beam position/velocity are integrated
	// continuously across the draw-ordered vector list and each segment is drawn as a short polyline
	// following the simulated trajectory. simulate_deflection fills out[] with DEFL_NOUT+1 points for
	// one segment (start S -> end E over draw time draw_secs) and advances the beam state; it returns
	// the trajectory in the same pixel space as the vertices. m_beam_valid resets each frame.
	static constexpr int DEFL_NOUT = 8;   // sub-quads per segment when deflection dynamics are on
	int simulate_deflection(float sx, float sy, float ex, float ey, double draw_secs, float *outx, float *outy);
	bool  m_beam_valid = false;
	float m_beam_px = 0.0f, m_beam_py = 0.0f;   // last actual beam position (pixels, drifted space)
	float m_beam_vx = 0.0f, m_beam_vy = 0.0f;   // last actual beam velocity (pixels / second)
	bool  m_defl_on = false;                    // deflection dynamics active this frame
	bool  m_glow_on = false;                    // analytic glow (extra wide gaussian quad) active this frame
	uint32_t m_vec_vpl = 18;                    // analytic verts per line this frame (incl. deflection / glow)
	// Glow buffer is packed: only components whose slider is active this frame get a 6-vertex slot, so a
	// chain using e.g. analytic_glow only emits 6 verts/line instead of the full 24 (the rest were
	// degenerate). Per-frame compacted slot offset for each component (vertex units; -1 = inactive),
	// and the resulting verts-per-line. Computed where m_glow_on is set, used by put_analytic_line and
	// the glow buffer allocation.
	int m_glow_off_glow  = -1;   // analytic glow dot / line gaussian (analytic_glow)
	int m_glow_off_ring  = -1;   // halation ring (ring_gain, point only)
	int m_glow_off_fill  = -1;   // halation inner fill (ring_fill, point only)
	int m_glow_off_flare = -1;   // overdrive white flare (intensity_overdrive)
	int m_glow_off_oglow = -1;   // overload-only bloom halo (overload_glow_gain)
	static constexpr int GLOW_RAY_SEGS = 3;   // taper sub-quads per starburst ray (bright thin base -> wide faint tip)
	bool m_caps_glow = false;    // cap_no_persist: line caps drawn into the separate no-persist FBO (bypass the phosphor pool)
	int m_glow_rays_n    = 0;    // number of rays packed per line (ray_count)
	int m_glow_vpl = 0;          // 6 * (active glow components), NOT including rays (see m_ray_vpl)
	// Starburst rays get their OWN buffer sized by POINT count (not visible_count like glow): a ray
	// costs 6 * ray_count * GLOW_RAY_SEGS verts, drawn only for hot dwell points, but reserving that
	// per LINE too (the old scheme) starved the transient vertex buffer in busy/text-heavy scenes
	// (thousands of lines x >100 verts each) and froze the glow buffer for the whole scene.
	int m_ray_vpl = 0;           // 6 * ray_count * GLOW_RAY_SEGS, 0 = rays off

	// Vector linearity calibration (integrator gain error, like the board's Linear pot): each vector
	// is drawn as (commanded vector) x gain from where the beam actually ended up, so the error
	// accumulates along a contiguous stroke and resets at a jump (non-contiguous start) or new frame.
	// m_lin_cmd_e* is the previous commanded endpoint (contiguity test); m_lin_drawn_e* the previous
	// drawn endpoint (where the next contiguous vector continues from).
	bool  m_lin_valid = false;
	float m_lin_cmd_ex = 0.0f, m_lin_cmd_ey = 0.0f;
	float m_lin_drawn_ex = 0.0f, m_lin_drawn_ey = 0.0f;

	// HDR composite (vector-hdr-display-study.md 4.1). An HDR-type chain (one declaring a
	// "screen_hdr" target) outputs linear light there. The vector screen is seeded into a linear
	// work target in absolute nits, then artwork/UI quads are drawn on top with their native MAME
	// blend modes (alpha / multiply / add) in linear light - reproducing the half-mirror combine -
	// and a final pass PQ-encodes the result (gamma on an SDR swapchain).
	bool m_vec_hdr_chain = false;          // active chain is HDR-type (has a screen_hdr target)
	bgfx_target *m_hdr_work = nullptr;     // linear work target (absolute nits): vector + artwork
	uint32_t m_hdr_work_view = UINT_MAX;   // per-frame view index the artwork/UI draws into
	bgfx_effect *m_hdr_gui_effect[4] = { nullptr, nullptr, nullptr, nullptr }; // per blend mode
	bgfx_effect *m_hdr_screen_effect = nullptr;   // seeds the work target from screen_hdr
	bgfx_effect *m_hdr_present_effect = nullptr;  // encodes the work target to the backbuffer

	std::map<uint32_t, rectangle_packer::packed_rectangle> m_hash_to_entry;
	std::vector<rectangle_packer::packable_rectangle> m_texinfo;
	rectangle_packer m_packer;

	uint32_t m_white[16*16];
	std::unique_ptr<bgfx_view> m_ortho_view;
	uint32_t m_max_view;

	bgfx_view *m_avi_view;
	avi_write *m_avi_writer;
	bgfx_target *m_avi_target;
	bgfx::TextureHandle m_avi_texture;
	bitmap_rgb32 m_avi_bitmap;
	uint8_t *m_avi_data;

	std::unique_ptr<util::xml::file> m_config;
	const util::notifier_subscription m_load_sub;
	const util::notifier_subscription m_save_sub;

	// Monitor glow: the vector device publishes the frame's shaped off-screen beam energy through
	// render_vector_stats; scaled by the chain's coefficient and injected into the monitor-glow
	// pass each frame. m_mglow_smoothed peak-tracks it (slow decay) to avoid vsync flicker.
	float m_mglow_smoothed = 0.0f;

	// Cyclic per-vector flicker (real AVG/DVG only): advances at a fixed real-time cadence
	// (flicker_period_ms) while the feature is active (busy scene), NOT once per present - so the
	// perceived flicker rate stays the same regardless of the actually-achieved present rate (a busy
	// scene running below full rate would otherwise cycle slower than a light one) and is comparable
	// across different machines/drivers. A paused/static busy scene still rotates through all
	// flicker_buckets over real elapsed time. See the "Cyclic per-vector flicker" comment in draw().
	uint64_t m_flicker_cycle = 0;
	int64_t m_flicker_last_hpc = 0;   // bx::getHPCounter() at the previous present (0 = not yet set)
	double m_flicker_accum_ms = 0.0;  // real ms accumulated toward the next bucket-cycle step
	// Perf: previous present's busyness/time-span stats, used INSTEAD of a dedicated pre-pass to
	// decide this present's flicker_busy/bucket (a one-present-stale count is imperceptible for a
	// chaotic cyclic effect). Updated from this frame's own primitive scan (drawbgfx.cpp, no extra
	// list traversal).
	int m_flicker_prev_count = 0;
	double m_flicker_prev_t0 = -1.0;
	double m_flicker_prev_t1 = -1.0;

	// True when a new emulated frame arrived at this present (render_vector_stats::frame_id
	// advanced since the previous present). Drives the chain's phosphor-tail freeze: re-presents
	// without emulation progress (pause, menu stills) must neither decay nor pump the pools.
	uint32_t m_vec_prev_frame_id = 0;
	bool m_vec_frame_advanced = false;
	// Resolution basis for beam width / bloom / defocus scaling. A ROT270 (portrait) vector screen is
	// pillarboxed in a wide window/fullscreen, so the raw framebuffer width over-scales the beam in
	// fullscreen vs a content-sized window. m_vec_extent_w peak-holds the vector bounding-box width (=
	// the real content width in fb px); m_vec_res_w is the per-present 1920-reference basis used by the
	// put_*_line magnitudes, so a given beam_width looks identical windowed and fullscreen.
	float m_vec_extent_w = 0.0f;
	float m_vec_res_w = 0.0f;
	// Identity of the active bgfx_chain the last time m_vec_extent_w was updated. The peak-hold's own
	// comment says it should "reset to 0 on FBO (re)create so a resolution change re-learns it" - but
	// no such reset actually existed anywhere in this file, so it never re-learned anything: switching
	// the active chain (e.g. color-phosphor -> monochrome-phosphor -> back to color-phosphor) does NOT
	// touch this peak (chain reload only recreates the CHAIN's own declared targets, not this renderer-
	// owned value), but the underlying game keeps running/drawing throughout, so if it happened to draw
	// wider content while a DIFFERENT chain was active, that peak silently carries over and permanently
	// changes color-phosphor's own width/bloom/defocus scale after switching back - with no game/
	// resolution change at all. Comparing screen_chain(0)'s pointer (changes on every reload_chains())
	// lets the peak-hold re-learn from scratch on any chain switch, matching the documented intent.
	bgfx_chain *m_vec_extent_chain = nullptr;
	// Emulated time (seconds) at the previous present, used to time-calibrate the max-persist
	// (Flicker Persist) decay: hold light for ~flicker_persist ms of emulated time, refresh-
	// independent. -1 = not yet sampled. dt==0 (paused / no emulation progress) holds the image.
	double m_vec_persist_prev_t = -1.0;

	// HV supply droop: the frame's total beam current loads the EHT supply, so a bright/busy frame
	// sags the high voltage - the whole picture dims and the spot defocuses, then recovers. The
	// frame total comes from render_vector_stats::total_energy; m_hv_smoothed peak-tracks it with
	// gentle decay (like the monitor glow) so it does not flicker against vsync; m_hv_load_norm is
	// the 0..1 normalised load the renderer applies. 0 load / hv_droop 0 = no effect.
	float m_hv_smoothed = 0.0f;
	float m_hv_load_norm = 0.0f;

	static const uint16_t CACHE_SIZE;
	static const uint32_t PACKABLE_SIZE;
	static const uint32_t WHITE_HASH;

	static uint32_t s_current_view;
	static uint32_t s_width[16];
	static uint32_t s_height[16];
};

#endif // MAME_RENDER_DRAWBGFX_H
