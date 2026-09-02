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
#include <string>
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

	uint32_t get_window_width() const { return m_new_dimensions.width(); }
	uint32_t get_window_height() const { return m_new_dimensions.height(); }

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
	bool drain_recording();
	void flush_recording();
	void release_recording();

	bool update_dimensions();
	void *native_window_handle() const;

	void setup_ortho_view();

	void allocate_buffer(render_primitive *prim, uint32_t blend, bgfx::TransientVertexBuffer *buffer);
	buffer_status buffer_primitives(bool atlas_valid, render_primitive** prim, bgfx::TransientVertexBuffer* buffer, int32_t screen, int window_index);

	void render_textured_quad(render_primitive* prim, bgfx::TransientVertexBuffer* buffer, int window_index);
	void set_hdr_gui_scale(bgfx_effect *effect, uint32_t blend, render_primitive const *prim);
	void render_vectrex_overlay_quad(render_primitive* prim, uint16_t view, int window_index);
	bool prepare_vectrex_overlay(bgfx_target *screen_hdr, float seed_peak, float paper_white, int window_index);
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

	// Speed/dwell density as a pure MODULATION of a device-supplied beam current, normalised to exactly
	// 1.0 at the reference sweep speed (energy_speed_norm) or reference dwell (energy_dot_ref), so a
	// chain can dial it in without moving the calibrated level. Lines run both ways (a fast sweep
	// deposits less per pixel, a slow one more) within [1/energy_line_max, energy_line_max]; dots only
	// ever boost ([1, energy_dot_max]) because the short-dwell side is already modelled by z_rise_tau.
	// Returns 1.0 when the model is off or the primitive carries no timestamps.

	// Port of the Vectrex driver's object_boost() (vectrex_v.cpp): a smooth per-object-type lift of
	// beam_energy, driven by display intensity - a bullet/explosion/star (typically drawn at higher
	// intensity than an enemy/ship) sails past energy_obj_knee and multiplies up to energy_obj_max,
	// with an extra energy_obj_star factor for point-classified (parked-dot) primitives. Model-derived
	// energy only; a device that supplies its own beam_energy already includes any such lift itself.
	// energy_obj_lift <= 0 = off (multiplier 1, unaffected). The caller applies this to POINTS ONLY
	// (see put_analytic_line) so it does not amplify fast multiplexed line content into a visible beat.
	float energy_object_lift(float intensity01, bool as_point) const;

	// Unified CRT beam jitter: one strength/time envelope drives energy and endpoint position together.
	// Endpoint noise is position-keyed, so shared vertices and coincident dot endpoints remain joined.
	void beam_jitter(float n, float x0, float y0, float x1, float y1,
		float &energy_scale, float &ox0, float &oy0, float &ox1, float &oy1);

	void set_bgfx_state(uint32_t blend);

	static uint32_t u32Color(uint32_t r, uint32_t g, uint32_t b, uint32_t a);

	bool check_for_dirty_atlas();
	bool update_atlas();
	// Atlas churn accounting; see drawbgfx.cpp. Call once per frame.
	void report_atlas_activity();
	// Vectrex overlay input tracing; see drawbgfx.cpp. Call once per frame.
	void report_vectrex_overlay_state();
	void process_atlas_packs(std::vector<std::vector<rectangle_packer::packed_rectangle>>& packed);
	uint32_t get_texture_hash(render_primitive *prim);

	void load_config(util::xml::data_node const &parentnode);
	void save_config(util::xml::data_node &parentnode);

	parent_module_holder m_module; // keep this where it will be destructed last

	bgfx_target *m_framebuffer;
	bgfx_texture *m_texture_cache;

	osd_dim m_dimensions; // Original display_mode
	osd_dim m_new_dimensions;

	// The host-rate vector presentation timer may ask for several presentations of the same
	// emulated frame. render_target::get_primitives() rebuilds and transforms the complete vector
	// list, so retain the list produced for the first presentation and reuse it for the additional
	// ones. The render target owns the list; this is a non-owning pointer valid until its next build.
	render_primitive_list *m_vector_present_primlist = nullptr;
	int m_vector_present_prim_w = 0;
	int m_vector_present_prim_h = 0;
	float m_vector_present_prim_aspect = 0.0f;
	bool m_vector_present_prim_transform = false;
	bool m_vector_present_prim_analytic = false;
	double m_vector_perf_prim_ms = 0.0;

	struct vector_perf_bucket
	{
		double prep_total_ms = 0.0;
		double prep_max_ms = 0.0;
		double prim_total_ms = 0.0;
		double prim_max_ms = 0.0;
		double frame_total_ms = 0.0;
		double frame_max_ms = 0.0;
		double scan_total_ms = 0.0;
		double scan_max_ms = 0.0;
		double analysis_total_ms = 0.0;
		double analysis_max_ms = 0.0;
		double energy_total_ms = 0.0;
		double energy_max_ms = 0.0;
		double cap_total_ms = 0.0;
		double cap_max_ms = 0.0;
		double convergence_total_ms = 0.0;
		double convergence_max_ms = 0.0;
		double geometry_total_ms = 0.0;
		double geometry_max_ms = 0.0;
		double submit_total_ms = 0.0;
		double submit_max_ms = 0.0;
		uint32_t count = 0;
	};
	vector_perf_bucket m_vector_perf[3]; // source, repeat with chain, cached repeat

	// Per-pass GPU accounting for -bgfx_debug, from bgfx::Stats::viewStats (which bgfx only
	// fills when BGFX_DEBUG_PROFILER is set and the backend supports timer queries).
	//
	// Keyed by the label bgfx_view_profile put on the view, NOT by view id: a pass lands on a
	// different id depending on which optional passes ran ahead of it that frame.
	//
	// gpu_busy is the sum of the per-view times for one frame. That is the honest "GPU was
	// working" figure. m_vector_perf_gpu_max_ms below is the frame's whole gpuTimeBegin..End
	// SPAN, which also covers the GPU sitting idle waiting for submissions, so it reads high
	// on a CPU-fed pipeline and must not be used to decide whether the GPU is saturated.
	struct view_gpu_bucket
	{
		std::string name;
		double total_ms = 0.0;
		double max_ms = 0.0;
		uint32_t frames = 0;
	};
	std::vector<view_gpu_bucket> m_view_gpu;
	std::vector<double> m_gpu_busy_ms;      // one entry per sampled frame, for avg/p95/max
	uint32_t m_gpu_view_frames_untimed = 0; // frames where no per-view timing was available
	// Dedup key for a repeated result set. ViewStats::gpuFrameNum would be the obvious choice
	// but the Metal backend never fills it in (renderer_mtl.h: "TODO: implement (currently
	// stays 0)"), so the first view's GPU begin timestamp stands in - it moves every time the
	// timer-query ring resolves something new, on every backend.
	int64_t m_gpu_last_view_begin = -1;

	int64_t m_vector_perf_window_hpc = 0;
	double m_vector_perf_gpu_max_ms = 0.0;
	double m_vector_perf_scan_ms = 0.0;
	double m_vector_perf_analysis_ms = 0.0;
	double m_vector_perf_energy_ms = 0.0;
	double m_vector_perf_cap_ms = 0.0;
	double m_vector_perf_convergence_ms = 0.0;
	double m_vector_perf_geometry_ms = 0.0;
	double m_vector_perf_submit_ms = 0.0;
	bool m_vec_chain_ran = false;
	bool m_vec_deposited_source = false;

	std::unique_ptr<texture_manager> m_textures;
	std::unique_ptr<target_manager> m_targets;
	std::unique_ptr<shader_manager> m_shaders;
	std::unique_ptr<effect_manager> m_effects;
	std::unique_ptr<chain_manager> m_chains;

	bgfx_effect *m_gui_effect[4];
	bgfx_effect *m_screen_effect[4];
	std::vector<uint32_t> m_seen_views;

	// FBO for vector drawing in the BGFX-sample style (not routed through chain_manager).
	// m_vec_render_scale scales the analytic vector FBO base and native vector-chain targets.
	// The final BGFX swapchain, UI and artwork remain at the physical window resolution.
	float m_vec_render_scale = 1.0f;
	// m_output_scale reduces the VecBeam HDR composite before a final full-window upscale.
	// The analytic vector path uses min(render, output), avoiding accidental double scaling.
	float m_output_scale = 1.0f;
	float m_vec_effective_scale = 1.0f;
	// m_vec_supersample is applied after m_vec_render_scale (both dimensions).
	uint16_t m_vec_supersample = 1;
	bgfx::FrameBufferHandle m_vec_fb = BGFX_INVALID_HANDLE;
	uint16_t m_vec_fb_w = 0;
	uint16_t m_vec_fb_h = 0;
	bool m_vectors_in_fbo = false;  // whether vector LINEs were drawn into the FBO this frame
	// Chain-driven opt-in: true while the active screen-0 chain declares "vector_engine":
	// "analytic" (see bgfx_chain::vector_engine). While false the vector FBOs are released and
	// vector LINE primitives take the stock buffer_primitives path untouched.
	bool m_vec_engine_active = false;
	// Emulated time (ms) at this present, cached once per frame for the unified Beam Jitter time axis.
	double m_vec_time_ms = 0.0;
	// Renderer energy-model aids, rebuilt by a per-frame pre-pass when active (see draw()):
	// - m_stroke_speed: per-primitive stroke-aggregate beam speed (px/ms) from cap_flags-delimited
	//   stroke runs (Vectrex RAMP strokes; sources without cap_flags never populate it), smoothing
	//   per-segment quantization noise along curves.
	// - m_dwell_scale: per-primitive energy scale (< 1) capping the additive pile-up of consecutive
	//   dots parked at one spot (the Vectrex driver model's "Dwell accum limit" equivalent).
	std::unordered_map<const render_primitive*, float> m_stroke_speed;
	std::unordered_map<const render_primitive*, float> m_dwell_scale;
	// Optional display-list arrival attenuation for untimed vector sources. Built once per frame from
	// cumulative segment length; values affect final light deposit only, never energy or beam width.

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
		float beam_jitter = 0.0f;  // unified energy + endpoint-position instability; 0 = off
		float beam_jitter_hz = 15.0f;
		float beam_jitter_saturation_start = 1.5f;
		float beam_jitter_saturation_range = 1.5f;
		float beam_jitter_saturation_curve = 2.0f;
		float overload_display_compression = 1.0f;
		float beam_width_max = 1.5f;
		float beam_width_min = 1.0f;
		float beam_width_over_scale = -1.0f;
		float beam_width_overmax = 4.0f;
		float phosphor_rgb_combination_width = 0.0f; // full RGB width gain; single primaries remain unchanged
		float bezel_long_threshold = 160.0f;
		float bright_normal_cap = 1.0f;
		float bright_sigmoid = 0.0f;
		float bright_sigmoid_center = 0.5f;
		float bright_threshold = 0.0f;
		float core_flat = 0.0f;
		float core_overlap_max = 0.0f; // colour chain: max-blend direct excitation to avoid overlap hotspots
		float convergence_bloom_gain = 0.0f;
		float convergence_bloom_falloff = 96.0f;
		float convergence_bloom_knee = 8.0f;
		float convergence_bloom_min_support = 110.0f;
		float convergence_bloom_source_radius = 0.0f;
		float convergence_bloom_threshold = 8.0f;
		float convergence_global_gain = 0.0f;
		float convergence_global_coverage = 0.55f;
		float deflection_damping = 0.5f;
		float deflection_dynamics = 0.0f;
		float deflection_settle = 5.0f;
		float dot_no_persist_dwell = 0.0f;
		float edge_defocus = 0.0f;
		float edge_defocus_curve = 2.0f;
		float energy_curve = 1.0f;
		// Density modulation applied ON TOP of a device-supplied beam current (0 = current only).
		// Separate from energy_infl, which still governs the fallback model for sources that supply
		// no current at all. Dots default to on: a parked beam really does deposit more.
		float energy_dot_curve = 1.6f;
		float energy_dot_max = 3.2f;
		float energy_dot_ref = 30.0f;
		float energy_dwell_cap = 16.0f;   // 16 = no cap (chains without the slider)
		float energy_infl = 0.6f;
		float energy_line_max = 4.0f;
		float energy_model = 0.0f;
		float energy_obj_knee = 0.75f;
		float energy_obj_lift = 0.0f;     // 0 = off (chains without the slider unchanged)
		float energy_obj_max = 3.0f;
		float energy_obj_sharp = 2.0f;
		float energy_obj_star = 1.5f;
		float energy_speed_norm = 0.8f;
		float energy_stroke_agg = 1.0f;
		float glow_narrow = 0.0f;
		float hv_droop = 0.0f;
		float hv_droop_dim = 1.0f;
		float hv_droop_onset = 0.0f;
		float hv_droop_ref = 10.0f;
		float intensity_overdrive = 0.0f;
		float intensity_overdrive_curve = 2.0f;
		float mask_overdrive_flare = 0.0f; // colour chain: route hot core through shadow mask
		float z_rise_tau = 0.0f;   // Z rise-time (us); 0 = off. Dims brief-dwell dots (see put_analytic_line).
		float line_cap_brightness = 1.0f;
		float line_cap_intensity_curve = 0.0f;
		float line_cap_mode = 0.0f;        // 0 legacy, 1 blank transitions, 2 RAMP flags, 3 off
		float line_cap_min_size = 0.0f;
		float line_cap_size = 2.0f;         // = LINE_CAP_SIZE_PX
		float line_cap_width = 1.5f;
		float line_cap_overload_add = 0.0f; // overload-only full-width addition at the endpoint
		float line_cap_overload_curve = 4.0f; // >1 delays endpoint growth until near max overload
		float line_cap_transition = 8.0f;   // endpoint-width taper length at 1920-ref
		float line_cap_curve = 1.5f;        // endpoint-width taper power
		float line_point_threshold = 2.0f;  // = LINE_POINT_THRESHOLD
		float overdrive_core = 0.0f;
		float overdrive_sat_curve = 1.0f;
		float overload_bloom = 0.0f;
		float overload_dot_gain = 1.0f;
		float overload_glow_gain = 0.0f;
		float overload_glow_width = 40.0f;
		float overload_max = 0.0f;
		float overload_ramp = 0.0f;
		float overload_threshold = 1.0f;
		float vertex_dwell_drive_curve = 0.0f;
		float vertex_dwell_drive_onset = 0.0f;
		float vertex_dwell_ref = 0.0f;
		float vertex_dwell_overlap = 0.0f;
		float vertex_dwell_overlap_radius = 3.0f;
		float vertex_dwell_overlap_ref = 4.0f;
		float overload_width_add = -1.0f;
		float overload_width_bloom_link = 1.0f;
		float overload_width_center = 0.65f;
		float overload_width_steepness = 10.0f;
		float phosphor_overdrive = 0.0f;
		float isolated_dot_min_size = 0.0f;
		float point_width_scale = 1.0f;
		float point_brightness_scale = 1.0f;
		float ray_angle = 15.0f;
		float ray_count_rand = 0.0f;    // time-varying random suppression of individual rays (0 = all present)
		float ray_gain = 0.0f;
		float ray_length = 60.0f;
		float ray_length_rand = 0.0f;   // time-varying per-ray length wobble (0 = static pattern)
		float ray_var = 0.6f;
		float ray_width = 1.2f;
		float ring_fill = 0.0f;
		float ring_gain = 0.0f;
		float halation_gain = 1.0f;
		float ring_min_dwell = 0.0f;
		float ring_over_gain = 0.0f;
		float ring_radius = 24.0f;
		float ring_threshold = 0.0f;
		float ring_width = 3.0f;
		float vector_image_scale = 1.0f; // board/monitor X-Y SIZE scale, applied before phosphor-face clipping
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
	// Last seen state of the chain's Advanced toggle. Flipping it changes WHICH sliders the menu
	// publishes, so the list has to be marked dirty (see draw()).
	bool m_advanced_sliders_shown = false;
	void refresh_vec_slider_cache();
	// Resolve the chain's three colour primaries and hand them to the passes that use them.
	void inject_primary_basis();
	// Upload the halo pedestal/renormalisation matching m_halo_quad_extent.
	void set_halo_quad_edge(bgfx_effect *effect);
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
	// sets 0.5 to quarter the fill cost of broad analytic optical footprints.
	bgfx::FrameBufferHandle m_vec_glow_fb = BGFX_INVALID_HANDLE;
	// Explicit optical effects (halation rim/fill and starburst rays), sampled directly by
	// the final composite so Glow Tail Curve / Black Toe affect ordinary glow only.
	bgfx::FrameBufferHandle m_vec_optical_fb = BGFX_INVALID_HANDLE;
	uint16_t m_vec_glow_fb_w = 0;
	uint16_t m_vec_glow_fb_h = 0;
	// No-persist FBO: short-dwell junction dots are drawn here (bypassing the phosphor pool) so a
	// chain pass can add them back AFTER the pool - bright while drawn, no afterimage - without ever
	// feeding them into the narrow/wide glow cascade (which m_vec_glow_fb shares). Same size/format as
	// m_vec_glow_fb; colour-only. Exposed to the chain as "npglow".
	bgfx::FrameBufferHandle m_vec_np_fb = BGFX_INVALID_HANDLE;

	// Analytic-AA vector line effect (fs_vector_line). Draws vector LINEs into m_vec_fb.
	// The subsequent post-processing is handled by the chain (JSON).
	bgfx_effect* m_line_effect = nullptr;
	// -bgfx_vec_line_shader analytic: gaussian line integral renderer (erf closed form,
	// one 6-vertex body quad per line on AnalyticLineVertex).
	bool m_line_analytic = false;
	void put_analytic_line(render_primitive *prim, AnalyticLineVertex *vertex, AnalyticLineVertex *glow_vertex = nullptr, AnalyticLineVertex *optical_vertex = nullptr, AnalyticLineVertex *np_vertex = nullptr, AnalyticLineVertex *ray_vertex = nullptr, float start_cap = 1.0f, float end_cap = 1.0f, float round_start = 1.0f, float round_end = 1.0f, float end_gain_start = 1.0f, float end_gain_finish = 1.0f, float stroke_px_per_ms = -1.0f, float dwell_scale = 1.0f,
			// Gain for the SCATTERED-LIGHT outputs only (overdrive flare, analytic glow, halation ring,
			// starburst rays). Under the beam time window those routes are not windowed - they have no
			// persistence of their own - so they would show a whole pass's scatter while the body shows
			// one slice of it, which reads as far too much halation on a split pass. Passing the fraction
			// of the sweep deposited so far keeps scatter in step with the light it is scattering.
			// NOT applied to the no-persist dot: there the body vertex is degenerate and the dot lives
			// entirely in that buffer, so it is beam rather than scatter and belongs at full strength.
			float aux_scale = 1.0f);

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
	// How far past the line a HALO quad (analytic glow, overload halo, starburst ray, edge glow) is
	// expanded, in sigma. Their sigma runs to tens of pixels, so this is what the wide glow actually
	// costs in fill: on Star Wars vec_glow_mrt is 17% of the frame in play and 30% during the Death
	// Star explosion. The core stays at the compiled-in 3.5 (QUAD_EDGE_PEDESTAL in
	// fs_vector_line_analytic.sc); only the halos follow this, and the shader gets the matching
	// pedestal through u_halo_quad_edge so the profile still reaches zero exactly at the cut.
	float m_halo_quad_extent = 3.5f;
	// window_aux_ramp for this present: the fraction of the sweep deposited so far, which the
	// post-pool routes are scaled by so their scatter stays in step with the light scattering it.
	// It used to be baked into the aux vertices; now the aux geometry is built once per source pass
	// (see deposit_aux) and this arrives at the passes that sample those buffers instead. It has to
	// be applied BEFORE the wide-glow pyramid's first level, which reshapes with a power curve -
	// scaling after that is not the same thing.
	float m_vec_aux_ramp = 1.0f;
	bool  m_glow_on = false;                    // analytic glow (extra wide gaussian quad) active this frame
	bool  m_optical_separate = false;            // modern mono/Vectrex: explicit optics bypass glow shaping
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
	bool m_caps_glow = false;    // cap_no_persist: short-dwell junction dots bypass the phosphor pool
	int m_glow_rays_n    = 0;    // number of rays packed per line (ray_count)
	int m_glow_vpl = 0;          // ordinary glow components only
	int m_optical_vpl = 0;       // halation rim/fill components only
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
	bgfx_target *m_hdr_present_work = nullptr; // encoded composite before optional output upscale
	uint32_t m_hdr_work_view = UINT_MAX;   // per-frame view index the artwork/UI draws into
	bgfx_effect *m_hdr_gui_effect[4] = { nullptr, nullptr, nullptr, nullptr }; // per blend mode
	bgfx_effect *m_hdr_screen_effect = nullptr;   // seeds the work target from screen_hdr
	bgfx_effect *m_hdr_present_effect = nullptr;  // encodes the work target to the backbuffer
	bgfx_effect *m_hdr_upscale_effect = nullptr;  // encoded composite -> physical output blit
	float m_hdr_ui_nits_scale = 200.0f;     // UI stays at paper white
	float m_hdr_art_nits_scale = 200.0f;    // ordinary artwork follows Room Ambient
	// Vectrex two-sided transparent overlay. Rear white ink and coloured resin/transmission are
	// rendered to optical masks and composed with screen_hdr.  Surface print images remain ordinary
	// bezel/artwork elements; the coloured-resin role itself is consumed by this optical path.
	bool m_vectrex_overlay_active = false;
	// Last reported reason the overlay path stood down, so the notice fires on change only.
	const char *m_vectrex_overlay_bail_reason = nullptr;
	bool m_vx_seen_active = false, m_vx_reported_active = false;
	bool m_vx_reported_ui_items = false, m_vx_reported_timer_with_ui = false;
	uint32_t m_vx_seen_role_quads = 0, m_vx_reported_role_quads = ~uint32_t(0);
	uint32_t m_vx_seen_plain_quads = 0, m_vx_reported_plain_quads = ~uint32_t(0);
	float m_vx_seen_seed_peak = 0.0f, m_vx_reported_seed_peak = -1.0f;
	float m_vx_seen_paper_white = 0.0f, m_vx_reported_paper_white = -1.0f;
	float m_vx_seen_ambient = 0.0f, m_vx_reported_ambient = -1.0f;
	bgfx_target *m_vectrex_overlay_white = nullptr;
	bgfx_target *m_vectrex_overlay_color = nullptr;
	bgfx_target *m_vectrex_overlay_blur[2] = { nullptr, nullptr };
	bgfx_effect *m_vectrex_overlay_mask_effect = nullptr;
	bgfx_effect *m_vectrex_overlay_blur_effect = nullptr;
	bgfx_effect *m_vectrex_overlay_downsample_effect = nullptr;
	bgfx_effect *m_vectrex_overlay_composite_effect = nullptr;
	// Optional HDR luminance diagnostic. A read-back texture receives hdr_work once per sampling
	// interval; the CPU applies the exact present roll-off to report requested/post-rolloff nits.

	// Broad glass/face scatter emitted only by a macro convergence-bloom component. Coordinates are
	// full-window UV; gain is linear relative to one nominal beam and coverage is sigma/half-diagonal.
	float m_conv_global_x = 0.5f;
	float m_conv_global_y = 0.5f;
	float m_conv_global_gain = 0.0f;
	float m_conv_global_coverage = 0.55f;
	float m_conv_global_color[3] = { 1.0f, 1.0f, 1.0f };

	std::map<uint32_t, rectangle_packer::packed_rectangle> m_hash_to_entry;
	std::vector<rectangle_packer::packable_rectangle> m_texinfo;
	rectangle_packer m_packer;

	uint32_t m_white[16*16];
	std::unique_ptr<bgfx_view> m_ortho_view;
	uint32_t m_max_view;

	std::unique_ptr<bgfx_view> m_avi_view;
	std::unique_ptr<avi_write> m_avi_writer;
	bgfx_target *m_avi_target;
	bgfx::TextureHandle m_avi_texture;
	bitmap_rgb32 m_avi_bitmap;
	// bgfx::readTexture fills its destination buffer on the render thread, and only promises the
	// data is there once bgfx has reached the frame number it hands back - three frames later in
	// practice. A single buffer therefore cannot be requested and converted in the same frame, so
	// requests go into this ring and are converted when their frame comes up. Four slots covers
	// the three that can be in flight plus the one being requested.
	static constexpr int AVI_READBACK_SLOTS = 4;
	struct avi_readback
	{
		std::unique_ptr<uint8_t []> data;
		uint32_t ready_frame = 0;
	};
	avi_readback m_avi_readback[AVI_READBACK_SLOTS];
	int m_avi_readback_head;    // oldest request still in flight
	int m_avi_readback_count;   // requests in flight
	bool m_avi_autostart_done;  // -bgfx_avi_name recording has been started for this session

	bool m_atlas_seeded = false;   // the white texture has been packed at least once
	uint32_t m_atlas_repacks = 0;
	uint32_t m_atlas_pack_failures = 0;
	int64_t m_atlas_report_hpc = 0;

	std::unique_ptr<util::xml::file> m_config;
	// The stored configuration, kept verbatim when the running chain selection came from an
	// explicitly specified chain so save_config can write the stored one back untouched. Null
	// otherwise, including when there was no stored configuration to keep.
	std::unique_ptr<util::xml::file> m_config_stored;
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

	// Beam time window (beam_window slider), the physical alternative to cyclic flicker: instead of
	// dropping a rotating bucket, each present deposits only the slice of the sweep the beam actually
	// covered during one presentation interval, and the phosphor pool - which integrates those slices
	// back into a whole pass - holds the rest. Where a pass is longer than the presentation time it
	// receives, its tail is never deposited, and that is the flicker.
	//
	// The list handed to the renderer describes a pass that ALREADY COMPLETED (vg_flush runs only at
	// the next VGGO), so a window in absolute machine time can never intersect it. The window walks
	// the pass's own [sweep_t0, sweep_t1] instead, positioned by the emulated time elapsed since the
	// list was first presented - NOT by counting presents, which would quantise the time a pass
	// receives to the screen-update period. Deriving it from elapsed time also self-corrects when a
	// present is dropped, and freezes on its own while the machine is paused.
	double m_vec_window_base_time = -1.0;   // emulated seconds at this list's first present
	// Keyed on list_generation, NOT frame_id: a pass must keep its window walk for as long as its
	// list lives, and the VGGO cadence is not the screen refresh. Star Wars refreshes its vector
	// screen every 24.381 ms but issues VGGO every 6 to 11 IRQ periods (measured median 36.53 ms in
	// attract), so one list is presented across several screen updates - the stale ones. Keying on
	// frame_id gave a 34 ms sweep only 24 ms worth of window and permanently clipped its tail.
	uint32_t m_vec_window_generation = ~uint32_t(0);
	bool m_vec_window_mode = false;        // beam_window active this present (read by the later chain gate)
	// Hysteresis state for the sweep-versus-window-width test in draw(): engage above 1.25x the
	// window width, disengage below 1.0x, so a title whose spans straddle the threshold does not
	// flip the phosphor between per-present and vector_phosphor_rate cadence every pass.
	bool m_vec_window_engaged = false;
	// Last reported engage decision and window width, so the info-level notice in draw() fires on a
	// real change instead of every present.
	bool m_vec_window_notice_engaged = false;
	bool m_vec_window_notice_blocked = false;   // reported "on but unavailable" already
	double m_vec_window_notice_w = -1.0;
	// Each engage decision is reported once per window width, then the notices go quiet. Index by
	// the decision: [0] inert, [1] active. m_vec_window_notice_alternating suppresses the rest.
	bool m_vec_window_notice_seen[2] = { false, false };
	bool m_vec_window_notice_alternating = false;
	// -verbose accounting: one "BEAMWIN" line per pass reporting how many presents it received and
	// how much of it was deposited. deposited < total is expected - the VGGO cadence and the present
	// rate are not commensurate, so a long pass loses its tail, which IS the flicker signal - but
	// deposited > total never is, and glow must always equal total (the glow route is not windowed;
	// if it starts tracking deposited instead, bloom will blink).
	int m_vec_window_log_presents = 0;
	int m_vec_window_log_deposited = 0;
	int m_vec_window_log_total = 0;
	int m_vec_window_log_glow = 0;
	double m_vec_window_log_span = 0.0;

	// Bezel edge glow (render_vector_stats::edge_energy): exact on-window CRT screen rectangle in
	// window pixels, taken from the full-screen VECTORBUF background quad emitted by vector.cpp.
	// Unlike a bounding box learned from lit lines, this is stable on sparse/title/ranking screens.
	float m_edge_box_min_x = 1e9f, m_edge_box_min_y = 1e9f;
	float m_edge_box_max_x = -1e9f, m_edge_box_max_y = -1e9f;
	// Temporally-smoothed edge bins (instant attack, exponential release over edge_glow_persist ms):
	// the raw per-frame bins follow the beam sweep pattern frame by frame and flicker hard; the real
	// glow is smoothed by phosphor/scatter persistence and the eye's integration. [4][EDGE_GLOW_BINS]
	// (size static_asserted against render_vector_stats in drawbgfx.cpp).
	float m_edge_smooth[4][16] = {};

	// True when a new emulated frame arrived at this present (render_vector_stats::frame_id
	// advanced since the previous present). Drives the chain's phosphor-tail freeze: re-presents
	// without emulation progress (pause, menu stills) must neither decay nor pump the pools.
	uint32_t m_vec_prev_frame_id = ~uint32_t(0);
	bool m_vec_frame_advanced = false;
	float m_vec_bezel_threshold_drawn = -1.0f;
	int m_vec_cached_vector_count = 0; // retained-list count reused by host-rate re-presents
	uint16_t m_vec_cached_content_w = 0;
	uint16_t m_vec_cached_content_h = 0;
	uint32_t m_vec_playback_reset = 0; // last MVEC discontinuity serial whose temporal history was cleared
	// Exact VECTORBUF face dimensions after layout, rotation and aspect-fit transforms. Spot-size
	// controls retain their historical "pixels at a 1920-wide 4:3 face" calibration: on a height-fit
	// display the physical height is converted to its 4:3-equivalent width; on a width-fit display the
	// physical width is already that reference. This makes the scale independent of game orientation
	// without changing the established landscape 4:3 defaults.
	float m_vec_res_w = 0.0f;
	float m_vec_res_h = 0.0f;
	float vec_res_scale()
	{
		const uint32_t si = window().index();
		const float window_w = std::max(1.0f, float(s_width[si]));
		const float window_h = std::max(1.0f, float(s_height[si]));
		const float fill_w = m_vec_res_w / window_w;
		const float fill_h = m_vec_res_h / window_h;
		const float reference_w = (fill_h + 1.0e-4f >= fill_w) ? (m_vec_res_h * (4.0f / 3.0f)) : m_vec_res_w;
		return reference_w / 1920.0f;
	}
	// Emulated time (seconds) at the previous present, used to time-calibrate the max-persist
	// (Flicker Persist) decay: hold light for ~flicker_persist ms of emulated time, refresh-
	// independent. -1 = not yet sampled. dt==0 (paused / no emulation progress) holds the image.
	double m_vec_persist_prev_t = -1.0;
	// Token budget for the expensive phosphor/monitor chain. Additional presentation-only
	// frames may consume at most vector_phosphor_rate tokens per second; emulation source
	// frames are always processed immediately and may temporarily borrow one token.
	double m_vec_phosphor_budget = 1.0;
	int64_t m_vec_phosphor_last_hpc = 0;

	// HV supply droop: excess energy from line primitives above overload_threshold loads the EHT
	// supply, so a mass-overload frame dims the whole picture and defocuses the spot, then recovers.
	// m_hv_smoothed peak-tracks the per-frame excess; m_hv_load_norm is the 0..1 onset-gated load.
	// Ordinary lines and isolated overload below hv_droop_onset have exactly no effect.
	float m_hv_smoothed = 0.0f;
	float m_hv_load_norm = 0.0f;

	static const uint16_t CACHE_SIZE;
	static const uint32_t PACKABLE_SIZE;
	static const uint32_t WHITE_HASH;

	static uint32_t s_current_view;

	// Window pixel size, per window index. MAME 0.289 retired these in favour of the per-instance
	// m_dimensions / m_new_dimensions, which the stock code now uses throughout. The vector path
	// keeps them because it reads window 0's size from code that is not window 0's renderer (the
	// chain configuration and the AVI display copy), which a per-instance member cannot express.
	// Kept in step with m_new_dimensions - assigned in create() and in the same per-frame poll -
	// so s_width[own index] and m_new_dimensions.width() are always the same value.
	static uint32_t s_width[16];
	static uint32_t s_height[16];
};

#endif // MAME_RENDER_DRAWBGFX_H
