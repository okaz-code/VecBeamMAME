// license:BSD-3-Clause
// copyright-holders:Miodrag Milanovic,Ryan Holtz,Dario Manesku,Branimir Karadzic,Aaron Giles
//============================================================
//
//  drawbgfx.cpp - BGFX renderer
//
//============================================================

#include "drawbgfx.h"

#include <chrono>

// render/bgfx
#include "bgfx/effect.h"
#include "bgfx/effectmanager.h"
#include "bgfx/shadermanager.h"
#include "bgfx/slider.h"
#include "bgfx/target.h"
#include "bgfx/target.h"
#include "bgfx/targetmanager.h"
#include "bgfx/texture.h"
#include "bgfx/texturemanager.h"
#include "bgfx/uniform.h"
#include "bgfx/view.h"

// render
#include "aviwrite.h"
#include "bgfxutil.h"
#include "render_module.h"

// emu
#include "emu.h"
#include "config.h"
#include "render.h"
#include "rendutil.h"
#include "ui/uimain.h"

// complete slider_state type (for core->description access)
#include "../frontend/mame/ui/slider.h"

// util
#include "util/xmlfile.h"

// OSD
#include "modules/lib/osdobj_common.h"
#include "window.h"

#include <bx/math.h>
#include <bx/readerwriter.h>
#include <bx/timer.h>

#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)
// standard windows headers
#include <windows.h>
#include <dxgi1_6.h>   // HDR display peak query (IDXGIOutput6::GetDesc1)
#if defined(SDLMAME_WIN32) && !defined(SDLMAME_SDL3)
#include <SDL2/SDL_syswm.h>
#endif
#else
#if defined(OSD_MAC)
extern void *GetOSWindow(void *wincontroller);
#else
#ifndef SDLMAME_SDL3
#include <SDL2/SDL_syswm.h>
#endif
#endif
#if defined(__APPLE__)
// EDR headroom query via the Objective-C runtime (NSScreen); avoids adding a .mm to the build.
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#endif

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include "imgui/imgui.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_map>


//============================================================
//  Renderer interface to parent module
//============================================================

class renderer_bgfx::parent_module
{
public:
	util::xml::data_node &persistent_settings() { return *m_persistent_settings; }
	osd_options const &options() const { return *m_options; }
	uint32_t max_texture_size() const { return m_max_texture_size; }
	// HDR auto-config: display peak (nits) resolved from bgfx_hdr_display_peak at library init
	// (numeric, or OS query for "auto"); 0 = feature off. Renderers hand it to their chain_manager.
	float hdr_display_peak_nits() const { return m_hdr_display_peak_nits; }
	// Effective SDR reference/paper white. On Windows HDR this follows the OS SDR-content level;
	// elsewhere it retains -bgfx_hdr_paper_white because no absolute SDR-white API is available.
	float paper_white_nits() const { return m_effective_paper_white_nits; }
	float detected_sdr_white_nits() const { return m_sdr_white_nits; }

	template <typename T>
	util::notifier_subscription subscribe_load(void (T::*func)(util::xml::data_node const &), T *obj)
	{
		return m_load_notifier.subscribe(delegate<void (util::xml::data_node const &)>(func, obj));
	}

	template <typename T>
	util::notifier_subscription subscribe_save(void (T::*func)(util::xml::data_node &), T *obj)
	{
		return m_save_notifier.subscribe(delegate<void (util::xml::data_node &)>(func, obj));
	}

protected:
	parent_module()
		: m_options(nullptr)
		, m_max_texture_size(0)
		, m_renderer_count(0)
	{
	}
	virtual ~parent_module()
	{
		assert(!m_renderer_count);
	}

	bool has_active_renderers() const
	{
		return 0 < m_renderer_count;
	}

	util::notifier<util::xml::data_node const &> m_load_notifier;
	util::notifier<util::xml::data_node &> m_save_notifier;
	util::xml::file::ptr m_persistent_settings;
	osd_options const *m_options;
	uint32_t m_max_texture_size;
	float m_hdr_display_peak_nits = 0.0f;   // see hdr_display_peak_nits()
	float m_sdr_white_nits = 0.0f;
	float m_effective_paper_white_nits = 200.0f;

private:
	friend class parent_module_holder;

	virtual void last_renderer_destroyed() = 0;

	void renderer_created()
	{
		++m_renderer_count;
	}

	void renderer_destroyed()
	{
		assert(m_renderer_count);
		if (!--m_renderer_count)
			last_renderer_destroyed();
	}

	unsigned m_renderer_count;
};


inline renderer_bgfx::parent_module_holder::parent_module_holder(parent_module &parent)
	: m_parent(parent)
{
	m_parent.renderer_created();
}


inline renderer_bgfx::parent_module_holder::~parent_module_holder()
{
	m_parent.renderer_destroyed();
}



//============================================================
//  OSD MODULE
//============================================================

// HDR PoC: true while the swapchain runs in HDR10 (PQ / Rec.2020, RGB10A2). Set at library
// init from -bgfx_hdr and the device caps; every later bgfx::reset must carry the same flags.
static bool s_bgfx_hdr_active = false;
// macOS EDR: true while HDR is active AND the backend is Metal. Metal does not use HDR10/PQ; the
// swapchain runs as an extended-linear float layer (RGBA16F + extendedLinearSRGB) and the present
// pass outputs linear values (1.0 = SDR white) instead of PQ. Selects the EDR branch in
// fs_vector_hdr_present (u_hdr_params.w). Set once after init alongside s_bgfx_hdr_active.
static bool s_bgfx_edr_active = false;

namespace osd {

namespace {

class video_bgfx : public osd_module, public render_module, protected renderer_bgfx::parent_module
{
public:
	video_bgfx()
		: osd_module(OSD_RENDERER_PROVIDER, "bgfx")
		, m_bgfx_library_initialized(false)
	{
	}
	~video_bgfx() { exit(); }

	virtual int init(osd_interface &osd, osd_options const &options) override;
	virtual void exit() override;

	virtual std::unique_ptr<osd_renderer> create(osd_window &window) override;

protected:
	virtual unsigned flags() const override { return FLAG_INTERACTIVE; }

private:
	virtual void last_renderer_destroyed() override;

	void load_config(config_type cfg_type, config_level cfg_level, util::xml::data_node const *parentnode);
	void save_config(config_type cfg_type, util::xml::data_node *parentnode);

	bool init_bgfx_library(osd_window &window);
	void resolve_hdr_display_peak(void *nwh);

	static bool set_platform_data(bgfx::PlatformData &platform_data, osd_window const &window);

	bool m_bgfx_library_initialized;
};


//============================================================
//  video_bgfx::init
//============================================================

int video_bgfx::init(osd_interface &osd, osd_options const &options)
{
	m_options = &options;
	m_persistent_settings = util::xml::file::create();

	// Check that BGFX directory exists
	char const *const bgfx_path = options.bgfx_path();
	osd::directory::ptr directory = osd::directory::open(bgfx_path);
	if (!directory)
	{
		osd_printf_error("Unable to find the BGFX path %s, please install it or fix the bgfx_path setting to use the BGFX renderer.\n", bgfx_path);
		return -1;
	}
	directory.reset();

	// Verify baseline shaders
	const bool gui_opaque_valid = effect_manager::validate_effect(options, "gui_opaque");
	const bool gui_blend_valid = effect_manager::validate_effect(options, "gui_blend");
	const bool gui_multiply_valid = effect_manager::validate_effect(options, "gui_multiply");
	const bool gui_add_valid = effect_manager::validate_effect(options, "gui_add");
	const bool all_gui_valid = gui_opaque_valid && gui_blend_valid && gui_multiply_valid && gui_add_valid;

	const bool screen_opaque_valid = effect_manager::validate_effect(options, "screen_opaque");
	const bool screen_blend_valid = effect_manager::validate_effect(options, "screen_blend");
	const bool screen_multiply_valid = effect_manager::validate_effect(options, "screen_multiply");
	const bool screen_add_valid = effect_manager::validate_effect(options, "screen_add");
	const bool all_screen_valid = screen_opaque_valid && screen_blend_valid && screen_multiply_valid && screen_add_valid;

	if (!all_gui_valid || !all_screen_valid)
	{
		osd_printf_error("BGFX: Unable to load required shaders. Please update the %s folder or adjust your bgfx_path setting.\n", options.bgfx_path());
		return -1;
	}

	m_max_texture_size = 16384; // Relatively safe default on modern GPUs

	// Register configuration handlers - do this last because it can't be undone
	downcast<osd_common_t &>(osd).machine().configuration().config_register(
			"bgfx",
			configuration_manager::load_delegate(&video_bgfx::load_config, this),
			configuration_manager::save_delegate(&video_bgfx::save_config, this));

	return 0;
}


//============================================================
//  video_bgfx::exit
//============================================================

void video_bgfx::exit()
{
	assert(!has_active_renderers());

	if (m_bgfx_library_initialized)
	{
		osd_printf_verbose("Shutting down BGFX library\n");
		imguiDestroy();
		bgfx::shutdown();
		m_bgfx_library_initialized = false;
	}
	m_max_texture_size = 0;
	m_persistent_settings.reset();
	m_options = nullptr;
}


//============================================================
//  video_bgfx::create
//============================================================

std::unique_ptr<osd_renderer> video_bgfx::create(osd_window &window)
{
	// start BGFX if this is the first window
	if (!m_bgfx_library_initialized)
	{
		assert(window.index() == 0); // bad things will happen otherwise
		assert(!has_active_renderers());

		osd_printf_verbose("Initializing BGFX library\n");
		if (!init_bgfx_library(window))
		{
			osd_printf_error("BGFX library initialization failed\n");
			return nullptr;
		}
		m_bgfx_library_initialized = true;
	}

	return std::make_unique<renderer_bgfx>(window, static_cast<renderer_bgfx::parent_module &>(*this));
}


//============================================================
//  video_bgfx::last_renderer_destroyed
//============================================================

void video_bgfx::last_renderer_destroyed()
{
	if (m_bgfx_library_initialized)
	{
		osd_printf_verbose("No more renderers - shutting down BGFX library\n");
		imguiDestroy();
		bgfx::shutdown();
		m_bgfx_library_initialized = false;
		m_max_texture_size = 0;
	}
}


//============================================================
//  video_bgfx::load_config
//============================================================

void video_bgfx::load_config(config_type cfg_type, config_level cfg_level, util::xml::data_node const *parentnode)
{
	if ((cfg_type == config_type::SYSTEM) && parentnode)
		m_load_notifier(*parentnode);
}


//============================================================
//  video_bgfx::save_config
//============================================================

void video_bgfx::save_config(config_type cfg_type, util::xml::data_node *parentnode)
{
	if (cfg_type == config_type::SYSTEM)
		m_save_notifier(*parentnode);
}


#if defined(__APPLE__)
static float detect_edr_headroom(void *nwh);
#endif
#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)
static bool detect_windows_hdr_active(void *nwh);
#endif

//============================================================
//  video_bgfx::init_bgfx_library
//============================================================

bool video_bgfx::init_bgfx_library(osd_window &window)
{
	osd_dim const wdim = window.get_size_pixels();
	s_bgfx_hdr_active = false;
	s_bgfx_edr_active = false;

	bgfx::Init init;
	init.type = bgfx::RendererType::Count;
	init.vendorId = BGFX_PCI_ID_NONE;
	init.resolution.width = wdim.width();
	init.resolution.height = wdim.height();
	init.resolution.numBackBuffers = 1;
	init.resolution.reset = video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
	if (!set_platform_data(init.platformData, window))
	{
		osd_printf_error("Setting BGFX platform data failed\n");
		return false;
	}

	std::string_view const backend(m_options->bgfx_backend());
	if (backend == "auto")
		; // do nothing
	else if (backend == "dx9" || backend == "d3d9")
		init.type = bgfx::RendererType::Direct3D9;
	else if (backend == "dx11" || backend == "d3d11")
		init.type = bgfx::RendererType::Direct3D11;
	else if (backend == "dx12" || backend == "d3d12")
		init.type = bgfx::RendererType::Direct3D12;
	else if (backend == "gles")
		init.type = bgfx::RendererType::OpenGLES;
	else if (backend == "glsl" || backend == "opengl")
		init.type = bgfx::RendererType::OpenGL;
	else if (backend == "vulkan")
		init.type = bgfx::RendererType::Vulkan;
	else if (backend == "metal")
		init.type = bgfx::RendererType::Metal;
	else
		osd_printf_warning("Unknown BGFX backend type '%s', going with auto-detection.\n", backend);

	// On Windows, creating an SDR swapchain first and changing it to HDR10 after bgfx::init makes
	// Windows Auto HDR classify the process as an SDR game and show its notification.  Preflight the
	// target monitor and start D3D11/D3D12 (including Windows auto-selection) in HDR10 immediately.
	// Other displays/backends retain the safe SDR bootstrap and the post-init capability fallback.
	bool initial_hdr = false;
#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)
	const bool d3d_hdr_backend = backend == "auto"
		|| init.type == bgfx::RendererType::Direct3D11
		|| init.type == bgfx::RendererType::Direct3D12;
	initial_hdr = m_options->bgfx_hdr() && d3d_hdr_backend && detect_windows_hdr_active(init.platformData.nwh);
	if (initial_hdr)
	{
		init.resolution.reset |= BGFX_RESET_HDR10;
		init.resolution.format = bgfx::TextureFormat::RGB10A2;
		osd_printf_verbose("BGFX: Windows HDR is active; creating the initial swapchain as HDR10\n");
	}
#endif

	if (!bgfx::init(init))
		return false;

	// Enable HDR only after the real renderer/output capability is known. This makes HDR-on-by-default
	// safe for SDR monitors and unsupported backends: they keep the already-created SDR swapchain.
	const bool hdr_requested = m_options->bgfx_hdr();
	s_bgfx_hdr_active = hdr_requested && ((bgfx::getCaps()->supported & BGFX_CAPS_HDR10) != 0);
	if (hdr_requested && !s_bgfx_hdr_active)
		osd_printf_warning("BGFX: HDR/EDR requested but unavailable; using the SDR swapchain\n");

	// macOS uses an extended-linear Metal layer rather than HDR10/PQ. The Metal capability means the
	// API exists; current headroom must also exceed 1.0 or this particular screen is effectively SDR.
	s_bgfx_edr_active = s_bgfx_hdr_active && (bgfx::getRendererType() == bgfx::RendererType::Metal);
#if defined(__APPLE__)
	if (s_bgfx_edr_active && detect_edr_headroom(init.platformData.nwh) <= 1.0f)
	{
		osd_printf_warning("BGFX: Metal EDR has no current display headroom; using SDR output\n");
		s_bgfx_edr_active = false;
		s_bgfx_hdr_active = false;
	}
#endif

	// Explicit confirmation (non-visual). With -bgfx_hdr the renderer is in one of three states; print
	// which, so EDR/HDR10 can be verified from the log instead of by eye (-verbose). The display still
	// has to grant headroom for HDR to actually show; that is a hardware/OS condition checkable via
	// NSScreen.maximumExtendedDynamicRangeColorComponentValue > 1.0.
	if (m_options->bgfx_hdr())
	{
		if (s_bgfx_edr_active)
			osd_printf_verbose("BGFX: HDR present path = macOS EDR (Metal, extended-linear RGBA16F)\n");
		else if (s_bgfx_hdr_active)
			osd_printf_verbose("BGFX: HDR present path = HDR10 (PQ / Rec.2020, RGB10A2)\n");
		else
			osd_printf_verbose("BGFX: HDR present path = SDR fallback (HDR requested but unavailable)\n");
	}

	// Avoid recreating the swapchain when the preflight state already matches the final capability
	// result.  A reset remains necessary for non-Windows paths and for a failed/changed preflight.
	if (initial_hdr != s_bgfx_hdr_active)
	{
		bgfx::reset(wdim.width(), wdim.height(),
			(video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) | (s_bgfx_hdr_active ? BGFX_RESET_HDR10 : 0),
			s_bgfx_hdr_active ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::Count);
	}

	// Enable debug text if requested
	bool bgfx_debug = m_options->bgfx_debug();
	bgfx::setDebug(bgfx_debug ? BGFX_DEBUG_STATS : BGFX_DEBUG_TEXT);

	// Get actual maximum texture size
	bgfx::Caps const *const caps = bgfx::getCaps();
	m_max_texture_size = caps->limits.maxTextureSize;

	ScreenVertex::init();
	AnalyticLineVertex::init();

	imguiCreate();

	// HDR auto-config: resolve bgfx_hdr_display_peak (numeric nits or "auto" = OS query) now that the
	// backend is up and the native window handle is known. The result feeds chain_manager's derived
	// beam_peak_nits / hdr_rolloff_max defaults.
	resolve_hdr_display_peak(init.platformData.nwh);

	return true;
}


//============================================================
//  HDR auto-config: display peak luminance resolution
//============================================================

#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)

// Return whether Windows Advanced Color/HDR is enabled for the monitor containing this window.
// This is deliberately queried before bgfx creates its first swapchain so Auto HDR never observes
// a transient SDR presentation path when native HDR10 was requested.
static bool detect_windows_hdr_active(void *nwh)
{
	if (nwh == nullptr)
		return false;
	const HMONITOR monitor = MonitorFromWindow(reinterpret_cast<HWND>(nwh), MONITOR_DEFAULTTONEAREST);
	if (monitor == nullptr)
		return false;
	MONITORINFOEXW monitor_info = {};
	monitor_info.cbSize = sizeof(monitor_info);
	if (!GetMonitorInfoW(monitor, &monitor_info))
		return false;

	UINT32 path_count = 0, mode_count = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
		return false;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
		return false;
	paths.resize(path_count);

	for (const DISPLAYCONFIG_PATH_INFO &path : paths)
	{
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
			|| _wcsicmp(source.viewGdiDeviceName, monitor_info.szDevice) != 0)
			continue;

		DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
		color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
		color.header.size = sizeof(color);
		color.header.adapterId = path.targetInfo.adapterId;
		color.header.id = path.targetInfo.id;
		return DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS && color.advancedColorEnabled;
	}
	return false;
}

// Query the Windows Advanced Color SDR-content white level for the monitor containing this window.
// DISPLAYCONFIG_SDR_WHITE_LEVEL is expressed in thousandths of the canonical 80-nit SDR white, so
// nits = value * 80 / 1000. This inbox Win32 route avoids a Windows App SDK runtime dependency.
static float detect_sdr_white_nits(void *nwh)
{
	if (nwh == nullptr)
		return 0.0f;
	const HMONITOR monitor = MonitorFromWindow(reinterpret_cast<HWND>(nwh), MONITOR_DEFAULTTONEAREST);
	if (monitor == nullptr)
		return 0.0f;
	MONITORINFOEXW monitor_info = {};
	monitor_info.cbSize = sizeof(monitor_info);
	if (!GetMonitorInfoW(monitor, &monitor_info))
		return 0.0f;

	UINT32 path_count = 0, mode_count = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
		return 0.0f;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
		return 0.0f;
	paths.resize(path_count);

	for (const DISPLAYCONFIG_PATH_INFO &path : paths)
	{
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
			|| _wcsicmp(source.viewGdiDeviceName, monitor_info.szDevice) != 0)
			continue;

		DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
		white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
		white.header.size = sizeof(white);
		white.header.adapterId = path.targetInfo.adapterId;
		white.header.id = path.targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
			return 80.0f * float(white.SDRWhiteLevel) / 1000.0f;
	}
	return 0.0f;
}

// Ask DXGI for the panel's advertised peak luminance (DXGI_OUTPUT_DESC1.MaxLuminance) of the monitor
// the window is on. dxgi.dll is loaded dynamically so no import-library dependency is added; local
// IIDs avoid needing __uuidof/dxguid. Returns 0 on any failure.
static float detect_hdr_display_peak_nits(void *nwh)
{
	static const GUID guid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } }; // IID_IDXGIFactory1
	static const GUID guid_output6  = { 0x068346e8, 0xaaec, 0x4b84, { 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1 } }; // IID_IDXGIOutput6

	if (nwh == nullptr)
		return 0.0f;
	const HMONITOR monitor = MonitorFromWindow(reinterpret_cast<HWND>(nwh), MONITOR_DEFAULTTONEAREST);
	if (monitor == nullptr)
		return 0.0f;

	const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	if (dxgi == nullptr)
		return 0.0f;
	using create_factory_fn = HRESULT (WINAPI *)(REFIID, void **);
	const auto create_factory = reinterpret_cast<create_factory_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));

	float peak = 0.0f;
	IDXGIFactory1 *factory = nullptr;
	if (create_factory != nullptr && SUCCEEDED(create_factory(guid_factory1, reinterpret_cast<void **>(&factory))))
	{
		IDXGIAdapter1 *adapter = nullptr;
		for (UINT a = 0; peak == 0.0f && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++)
		{
			IDXGIOutput *output = nullptr;
			for (UINT o = 0; peak == 0.0f && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++)
			{
				IDXGIOutput6 *output6 = nullptr;
				if (SUCCEEDED(output->QueryInterface(guid_output6, reinterpret_cast<void **>(&output6))))
				{
					DXGI_OUTPUT_DESC1 desc;
					if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor)
						peak = desc.MaxLuminance;
					output6->Release();
				}
				output->Release();
			}
			adapter->Release();
		}
		factory->Release();
	}
	FreeLibrary(dxgi);
	return peak;
}

#elif defined(__APPLE__)

// EDR headroom (multiples of SDR reference white) of the window's screen, via the ObjC runtime so no
// .mm needs to be added to the build. nwh is the NSWindow under SDL/Metal. Returns 0 on failure.
static float detect_edr_headroom(void *nwh)
{
	using msg_id_fn = id (*)(id, SEL);
	using msg_dbl_fn = double (*)(id, SEL);
	using msg_resp_fn = BOOL (*)(id, SEL, SEL);

	id screen = nullptr;
	if (nwh != nullptr)
		screen = reinterpret_cast<msg_id_fn>(objc_msgSend)(reinterpret_cast<id>(nwh), sel_registerName("screen"));
	if (screen == nullptr)
		screen = reinterpret_cast<msg_id_fn>(objc_msgSend)(
				reinterpret_cast<id>(objc_getClass("NSScreen")), sel_registerName("mainScreen"));
	if (screen == nullptr)
		return 0.0f;

	// Prefer currently available headroom because it reflects brightness, display mode and thermal/
	// power limits.  Before the CAMetalLayer requests EDR, however, some macOS versions report 1.0
	// here even on an XDR display.  In that bootstrap state consult the potential value as capability;
	// otherwise the SDR decision prevents the EDR layer from ever being enabled (a circular test).
	const SEL current_sel = sel_registerName("maximumExtendedDynamicRangeColorComponentValue");
	float current = 0.0f;
	if (reinterpret_cast<msg_resp_fn>(objc_msgSend)(screen, sel_registerName("respondsToSelector:"), current_sel))
		current = float(reinterpret_cast<msg_dbl_fn>(objc_msgSend)(screen, current_sel));
	if (current > 1.0f)
		return current;

	const SEL potential_sel = sel_registerName("maximumPotentialExtendedDynamicRangeColorComponentValue");
	if (reinterpret_cast<msg_resp_fn>(objc_msgSend)(screen, sel_registerName("respondsToSelector:"), potential_sel))
	{
		const float potential = float(reinterpret_cast<msg_dbl_fn>(objc_msgSend)(screen, potential_sel));
		if (potential > current)
		{
			osd_printf_verbose("BGFX: macOS EDR current headroom %.2f; using potential headroom %.2f for bootstrap\n",
				current, potential);
			return potential;
		}
	}
	return current;
}

#endif

void video_bgfx::resolve_hdr_display_peak(void *nwh)
{
	m_hdr_display_peak_nits = 0.0f;
	m_sdr_white_nits = 0.0f;
	m_effective_paper_white_nits = float(std::max(1, m_options->bgfx_hdr_paper_white()));
#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)
	m_sdr_white_nits = detect_sdr_white_nits(nwh);
	if (m_sdr_white_nits > 0.0f)
	{
		osd_printf_verbose("BGFX: Windows SDR reference white resolved to %.1f nits\n", m_sdr_white_nits);
		// HDR10 is absolute-nit output, so anchor MAME UI/artwork white to the same level Windows
		// uses for SDR content. SDR output and macOS EDR remain relative and keep the option value.
		if (s_bgfx_hdr_active && !s_bgfx_edr_active)
			m_effective_paper_white_nits = m_sdr_white_nits;
	}
	else
		osd_printf_verbose("BGFX: Windows SDR reference white query unavailable; using %0.0f nits\n",
			m_effective_paper_white_nits);
#endif
	if (!m_options->bgfx_hdr())
		return;

	// Only meaningful when an HDR present path actually engaged. If the HDR request fell back to SDR
	// (no Windows HDR / wrong backend), applying an HDR-peak-derived beam_peak_nits would just dim the
	// SDR image, so leave the chain defaults alone.
	if (!s_bgfx_hdr_active && !s_bgfx_edr_active)
	{
		if (*m_options->bgfx_hdr_display_peak() && strcmp(m_options->bgfx_hdr_display_peak(), "0") != 0)
			osd_printf_verbose("BGFX: HDR fell back to SDR; ignoring bgfx_hdr_display_peak\n");
		return;
	}

	const std::string_view peak_opt = m_options->bgfx_hdr_display_peak();
	float peak = 0.0f;
	if (peak_opt == OSDOPTVAL_AUTO)
	{
#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)
		peak = detect_hdr_display_peak_nits(nwh);
		if (peak <= 0.0f)
			osd_printf_warning("BGFX: bgfx_hdr_display_peak auto: DXGI query failed; keeping chain HDR defaults\n");
#elif defined(__APPLE__)
		// EDR works in headroom (multiples of SDR white) rather than absolute nits. Converting through
		// paper white lets the shared auto-calibration keep normal beam/SDR-white ratio stable while
		// reserving the remaining EDR headroom for overlaps and overload.
		const float headroom = detect_edr_headroom(nwh);
		if (headroom > 1.0f)
		{
			osd_printf_verbose("BGFX: macOS current EDR headroom resolved to %.2fx SDR white\n", headroom);
			peak = headroom * m_effective_paper_white_nits;
		}
		else
			osd_printf_warning("BGFX: bgfx_hdr_display_peak auto: EDR headroom query failed; keeping chain HDR defaults\n");
#else
		osd_printf_warning("BGFX: bgfx_hdr_display_peak auto is not supported on this platform; keeping chain HDR defaults\n");
#endif

	}
	else
	{
		peak = float(atof(std::string(peak_opt).c_str()));
		if (peak < 0.0f)
			peak = 0.0f;
	}

	if (peak > 0.0f && peak < 400.0f)
		osd_printf_warning("BGFX: bgfx_hdr_display_peak %.0f nits is very low for an HDR display\n", peak);
	if (peak > 0.0f)
		osd_printf_verbose("BGFX: HDR display peak resolved to %.0f nits (%s)\n", peak,
				(peak_opt == OSDOPTVAL_AUTO) ? "auto-detected" : "from option");
	m_hdr_display_peak_nits = peak;
}


//============================================================
//  Utility for setting up window handle
//============================================================
#ifdef SDLMAME_SDL3
bool video_bgfx::set_platform_data(bgfx::PlatformData &platform_data, osd_window const &window)
{
#if defined(OSD_WINDOWS)
	platform_data.ndt = nullptr;
	platform_data.nwh = dynamic_cast<win_window_info const &>(window).platform_window();
#elif defined(OSD_MAC)
	platform_data.ndt = nullptr;
	platform_data.nwh = GetOSWindow(dynamic_cast<mac_window_info const &>(window).platform_window());
#elif defined(SDLMAME_EMSCRIPTEN)
	platform_data.ndt = nullptr;
	platform_data.nwh = (void *)"#canvas"; // HTML5 target selector
#else // defined(OSD_*)
	const auto winProps = SDL_GetWindowProperties(dynamic_cast<sdl_window_info const &>(window).platform_window());
#if defined(SDL_PLATFORM_WINDOWS)
							  platform_data.ndt = nullptr;
	platform_data.nwh = (HWND)SDL_GetPointerProperty(winProps, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#endif
#if defined(SDL_PLATFORM_MACOS)
	platform_data.ndt = nullptr;
	platform_data.nwh = SDL_GetPointerProperty(winProps, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
#endif
#if defined(SDL_PLATFORM_LINUX)
	if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0)
	{
		platform_data.ndt = (void *)SDL_GetPointerProperty(winProps, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
		platform_data.nwh = (void *)SDL_GetNumberProperty(winProps, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
	}
	else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)
	{
		platform_data.ndt = (struct wl_display *)SDL_GetPointerProperty(winProps, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
		platform_data.nwh = (struct wl_surface *)SDL_GetPointerProperty(winProps, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
		if (!platform_data.nwh)
		{
			osd_printf_error("BGFX: Error creating a Wayland window\n");
			return false;
		}
		platform_data.type = bgfx::NativeWindowHandleType::Wayland;
	}
#endif
#if defined(SDL_PLATFORM_ANDROID)
	platform_data.ndt = nullptr;
	platform_data.nwh = SDL_GetPointerProperty(winProps, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
#endif
#endif // defined(OSD_*)

	platform_data.context = nullptr;
	platform_data.backBuffer = nullptr;
	platform_data.backBufferDS = nullptr;
	bgfx::setPlatformData(platform_data);

	return true;
}
#else
bool video_bgfx::set_platform_data(bgfx::PlatformData &platform_data, osd_window const &window)
{
#if defined(OSD_WINDOWS)
	platform_data.ndt = nullptr;
	platform_data.nwh = dynamic_cast<win_window_info const &>(window).platform_window();
#elif defined(OSD_MAC)
	platform_data.ndt = nullptr;
	platform_data.nwh = GetOSWindow(dynamic_cast<mac_window_info const &>(window).platform_window());
#elif defined(SDLMAME_EMSCRIPTEN)
	platform_data.ndt = nullptr;
	platform_data.nwh = (void *)"#canvas"; // HTML5 target selector
#else // defined(OSD_*)
	SDL_SysWMinfo wmi;
	SDL_VERSION(&wmi.version);
	if (!SDL_GetWindowWMInfo(dynamic_cast<sdl_window_info const &>(window).platform_window(), &wmi))
	{
		osd_printf_error("BGFX: Error getting SDL window info: %s\n", SDL_GetError());
		return false;
	}

	switch (wmi.subsystem)
	{
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
	case SDL_SYSWM_WINDOWS:
		platform_data.ndt = nullptr;
		platform_data.nwh = wmi.info.win.window;
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
	case SDL_SYSWM_X11:
		platform_data.ndt = wmi.info.x11.display;
		platform_data.nwh = (void *)uintptr_t(wmi.info.x11.window);
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
	case SDL_SYSWM_COCOA:
		platform_data.ndt = nullptr;
		platform_data.nwh = wmi.info.cocoa.window;
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND) && SDL_VERSION_ATLEAST(2, 0, 16)
	case SDL_SYSWM_WAYLAND:
		platform_data.ndt = wmi.info.wl.display;
		platform_data.nwh = wmi.info.wl.surface;
		if (!platform_data.nwh)
		{
			osd_printf_error("BGFX: Error creating a Wayland window\n");
			return false;
		}
		platform_data.type = bgfx::NativeWindowHandleType::Wayland;
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_ANDROID)
	case SDL_SYSWM_ANDROID:
		platform_data.ndt = nullptr;
		platform_data.nwh = wmi.info.android.window;
		break;
#endif
	default:
		osd_printf_error("BGFX: Unsupported SDL window manager type %u\n", wmi.subsystem);
		return false;
	}
#endif // defined(OSD_*)

	platform_data.context = nullptr;
	platform_data.backBuffer = nullptr;
	platform_data.backBufferDS = nullptr;
	bgfx::setPlatformData(platform_data);

	return true;
}
#endif

} // anonymous namespace

} // namespace osd

MODULE_DEFINITION(RENDERER_BGFX, osd::video_bgfx)



//============================================================
//  CONSTANTS
//============================================================

uint16_t const renderer_bgfx::CACHE_SIZE = 1024;
uint32_t const renderer_bgfx::PACKABLE_SIZE = 128;
uint32_t const renderer_bgfx::WHITE_HASH = 0x87654321;
char const *const renderer_bgfx::WINDOW_PREFIX = "Window 0, ";



//============================================================
//  MACROS
//============================================================

#define GIBBERISH       (0)
#define SCENE_VIEW      (0)



//============================================================
//  STATICS
//============================================================

static uint32_t s_bgfx_frame_number = 0;

uint32_t renderer_bgfx::s_current_view = 0;
uint32_t renderer_bgfx::s_width[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
uint32_t renderer_bgfx::s_height[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };



//============================================================
//  helper for getting native platform window
//============================================================

#ifdef OSD_SDL
#ifdef SDLMAME_SDL3
static std::pair<void *, bool> sdlNativeWindowHandle(SDL_Window *window)
{
#if defined(SDL_PLATFORM_WIN32)
	return std::make_pair((HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL), true);
#endif
#if defined(SDLMAME_MACOSX)
	return std::make_pair(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL), true);
#endif
#if defined(SDL_PLATFORM_LINUX)
	if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0)
	{
		return std::make_pair((void *)uintptr_t(SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)), true);
	}
	else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0)
	{
		return std::make_pair((struct wl_surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL), true);
	}
#endif
#if defined(SDL_PLATFORM_ANDROID)
		return std::make_pair(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL), true);
#endif
		return std::make_pair(nullptr, false);
}
#else
static std::pair<void *, bool> sdlNativeWindowHandle(SDL_Window *window)
{
	SDL_SysWMinfo wmi;
	SDL_VERSION(&wmi.version);
	if (!SDL_GetWindowWMInfo(window, &wmi))
		return std::make_pair(nullptr, false);

	switch (wmi.subsystem)
	{
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
	case SDL_SYSWM_WINDOWS:
		return std::make_pair(wmi.info.win.window, true);
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
	case SDL_SYSWM_X11:
		return std::make_pair((void *)uintptr_t(wmi.info.x11.window), true);
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
	case SDL_SYSWM_COCOA:
		return std::make_pair(wmi.info.cocoa.window, true);
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND) && SDL_VERSION_ATLEAST(2, 0, 16)
	case SDL_SYSWM_WAYLAND:
		return std::make_pair(wmi.info.wl.surface, true);
#endif
#if defined(SDL_VIDEO_DRIVER_ANDROID)
	case SDL_SYSWM_ANDROID:
		return std::make_pair(wmi.info.android.window, true);
#endif
	default:
		return std::make_pair(nullptr, false);
	}
}
#endif
#endif // OSD_SDL



//============================================================
//  renderer_bgfx - constructor
//============================================================

renderer_bgfx::renderer_bgfx(osd_window &window, parent_module &parent)
	: osd_renderer(window)
	, m_module(parent)
	, m_framebuffer(nullptr)
	, m_texture_cache(nullptr)
	, m_dimensions(0, 0)
	, m_max_view(0)
	, m_avi_view(nullptr)
	, m_avi_writer(nullptr)
	, m_avi_target(nullptr)
	, m_load_sub(parent.subscribe_load(&renderer_bgfx::load_config, this))
	, m_save_sub(parent.subscribe_save(&renderer_bgfx::save_config, this))
{
	// load settings if recreated after fullscreen toggle
	util::xml::data_node *windownode = m_module().persistent_settings().get_child("window");
	while (windownode)
	{
		if (windownode->get_attribute_int("index", -1) != window.index())
		{
			windownode = windownode->get_next_sibling("window");
		}
		else
		{
			if (!m_config)
			{
				m_config = util::xml::file::create();
				windownode->copy_into(*m_config);
			}
			std::exchange(windownode, windownode->get_next_sibling("window"))->delete_node();
		}
	}
}



//============================================================
//  renderer_bgfx - destructor
//============================================================

renderer_bgfx::~renderer_bgfx()
{
	// persist settings across fullscreen toggle
	if (m_config)
		m_config->get_first_child()->copy_into(m_module().persistent_settings());
	else if (m_chains)
		m_chains->save_config(m_module().persistent_settings());

	// release the vector-drawing FBO
	if (bgfx::isValid(m_vec_fb))
	{
		bgfx::destroy(m_vec_fb);
		m_vec_fb = BGFX_INVALID_HANDLE;
	}
	if (bgfx::isValid(m_vec_glow_fb))
	{
		bgfx::destroy(m_vec_glow_fb);
		m_vec_glow_fb = BGFX_INVALID_HANDLE;
	}
	if (bgfx::isValid(m_vec_optical_fb))
	{
		bgfx::destroy(m_vec_optical_fb);
		m_vec_optical_fb = BGFX_INVALID_HANDLE;
	}
	if (bgfx::isValid(m_vec_np_fb))
	{
		bgfx::destroy(m_vec_np_fb);
		m_vec_np_fb = BGFX_INVALID_HANDLE;
	}
	m_vec_fb_w = m_vec_fb_h = 0;
	m_vec_glow_fb_w = m_vec_glow_fb_h = 0;
	destroy_hdr_diagnostics();
	window().machine().ui().set_hdr_diagnostic_text(std::string());

	bgfx::reset(0, 0, BGFX_RESET_NONE);

	if (m_avi_writer != nullptr && m_avi_writer->recording())
	{
		m_avi_writer->stop();

		m_targets->destroy_target("avibuffer0");
		m_avi_target = nullptr;

		bgfx::destroy(m_avi_texture);

		delete m_avi_writer;
		delete [] m_avi_data;
		delete m_avi_view;
	}
}



//============================================================
//  renderer_bgfx::create
//============================================================

int renderer_bgfx::create()
{
	const osd_dim wdim = window().get_size_pixels();
	s_width[window().index()] = wdim.width();
	s_height[window().index()] = wdim.height();
	m_dimensions = wdim;

	// finish creating the renderer
	m_textures = std::make_unique<texture_manager>();
	m_targets = std::make_unique<target_manager>(*m_textures);

	if (window().index() != 0)
	{
#ifdef OSD_WINDOWS
		m_framebuffer = m_targets->create_backbuffer(dynamic_cast<win_window_info &>(window()).platform_window(), s_width[window().index()], s_height[window().index()]);
#elif defined(OSD_MAC)
		m_framebuffer = m_targets->create_backbuffer(GetOSWindow(dynamic_cast<mac_window_info &>(window()).platform_window()), s_width[window().index()], s_height[window().index()]);
#else
		auto const [winhdl, success] = sdlNativeWindowHandle(dynamic_cast<sdl_window_info &>(window()).platform_window());
		if (!success)
		{
			m_targets.reset();
			m_textures.reset();
			return -1;
		}
		m_framebuffer = m_targets->create_backbuffer(winhdl, s_width[window().index()], s_height[window().index()]);
#endif
		bgfx::touch(window().index());

		// Non-primary window: the HDR composite is window-0 only, so the UI view targets the
		// backbuffer as usual.
		if (m_ortho_view)
			m_ortho_view->set_backbuffer(m_framebuffer);
	}

	m_shaders = std::make_unique<shader_manager>();
	m_effects = std::make_unique<effect_manager>(*m_shaders);

	// Create program from shaders.
	m_gui_effect[0] = m_effects->get_or_load_effect(m_module().options(), "gui_opaque");
	m_gui_effect[1] = m_effects->get_or_load_effect(m_module().options(), "gui_blend");
	m_gui_effect[2] = m_effects->get_or_load_effect(m_module().options(), "gui_multiply");
	m_gui_effect[3] = m_effects->get_or_load_effect(m_module().options(), "gui_add");

	m_screen_effect[0] = m_effects->get_or_load_effect(m_module().options(), "screen_opaque");
	m_screen_effect[1] = m_effects->get_or_load_effect(m_module().options(), "screen_blend");
	m_screen_effect[2] = m_effects->get_or_load_effect(m_module().options(), "screen_multiply");
	m_screen_effect[3] = m_effects->get_or_load_effect(m_module().options(), "screen_add");

	const uint32_t max_prescale_size = std::min(2u * std::max(wdim.width(), wdim.height()), m_module().max_texture_size());
	m_chains = std::make_unique<chain_manager>(
			window().machine(),
			m_module().options(),
			*m_textures,
			*m_targets,
			*m_effects,
			window().index(),
			*this,
			window().prescale(),
			max_prescale_size);
	// HDR auto-config: hand over the resolved display peak before the first load_chains() so the
	// derived beam_peak_nits / hdr_rolloff_max act as defaults (cfg and live edits still win).
	m_chains->set_hdr_display_peak(m_module().hdr_display_peak_nits());
	m_chains->set_hdr_paper_white(m_module().paper_white_nits());
	m_sliders_dirty = true;

	uint32_t flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
	m_texture_cache = m_textures->create_texture("#cache", bgfx::TextureFormat::BGRA8, CACHE_SIZE, 0, CACHE_SIZE, nullptr, flags);

	memset(m_white, 0xff, sizeof(uint32_t) * 16 * 16);
	m_texinfo.push_back(rectangle_packer::packable_rectangle(WHITE_HASH, PRIMFLAG_TEXFORMAT(TEXFORMAT_ARGB32), 16, 16, 16, nullptr, m_white));

	// The vector-drawing FBOs (window 0 only) are created lazily by draw()'s recreate-on-mismatch
	// logic, and only while the active chain opts into the analytic vector engine (the chain JSON's
	// "vector_engine": "analytic" key - chains are not loaded yet at this point). Chains without
	// the key never allocate them and vector games render through the stock path untouched.
	if (window().index() == 0)
	{
		const int ss = m_module().options().bgfx_vec_supersample();
		m_vec_supersample = uint16_t(ss < 1 ? 1 : (ss > 2 ? 2 : ss));
		m_vec_render_scale = std::clamp(m_module().options().bgfx_render_scale(), 0.1f, 1.0f);
		osd_printf_verbose("BGFX: analytic vector render scale %.2f, supersample %u (effective raster scale %.2f)\n",
			m_vec_render_scale, unsigned(m_vec_supersample), m_vec_render_scale * float(m_vec_supersample));
	}

	// load the analytic-AA vector line effect
	if (window().index() == 0)
	{
		const char *const line_shader = m_module().options().bgfx_vec_line_shader();
		m_line_analytic = (line_shader != nullptr && strcmp(line_shader, "analytic") == 0);
		m_line_effect = m_effects->get_or_load_effect(m_module().options(),
			m_line_analytic ? "vector/vector_line_analytic" : "vector/vector_line");
		if (m_line_analytic && m_line_effect == nullptr)
		{
			osd_printf_warning("BGFX: analytic line effect failed to load, falling back to classic\n");
			m_line_analytic = false;
			m_line_effect = m_effects->get_or_load_effect(m_module().options(), "vector/vector_line");
		}

		// HDR composite effects (loaded unconditionally: an HDR-type chain composites even on an
		// SDR swapchain, where the present pass gamma-encodes instead of PQ).
		m_hdr_screen_effect  = m_effects->get_or_load_effect(m_module().options(), "vector/vector_hdr_screen");
		m_hdr_present_effect = m_effects->get_or_load_effect(m_module().options(), "vector/vector_hdr_present");
		m_hdr_gui_effect[BLENDMODE_NONE]         = m_effects->get_or_load_effect(m_module().options(), "vector/hdr_gui_opaque");
		m_hdr_gui_effect[BLENDMODE_ALPHA]        = m_effects->get_or_load_effect(m_module().options(), "vector/hdr_gui_blend");
		m_hdr_gui_effect[BLENDMODE_RGB_MULTIPLY] = m_effects->get_or_load_effect(m_module().options(), "vector/hdr_gui_multiply");
		m_hdr_gui_effect[BLENDMODE_ADD]          = m_effects->get_or_load_effect(m_module().options(), "vector/hdr_gui_add");

		// Vector frame statistics (frame counter, list staleness, total / off-screen beam energy)
		// arrive through the render layer (render_vector_stats, published by the vector device
		// into its container and propagated onto the primitive list) - the renderer no longer
		// reaches into the device itself.
	}

	return 0;
}



//============================================================
//  renderer_bgfx::record
//============================================================

void renderer_bgfx::record()
{
	if (window().index() > 0)
		return;

	if (m_avi_writer == nullptr)
	{
		m_avi_writer = new avi_write(window().machine(), s_width[0], s_height[0]);
		m_avi_data = new uint8_t[s_width[0] * s_height[0] * 4];
		m_avi_bitmap.allocate(s_width[0], s_height[0]);
	}

	if (m_avi_writer->recording())
	{
		m_avi_writer->stop();
		m_targets->destroy_target("avibuffer0");
		m_avi_target = nullptr;
		bgfx::destroy(m_avi_texture);
		delete m_avi_view;
		m_avi_view = nullptr;
	}
	else
	{
		m_avi_writer->record(m_module().options().bgfx_avi_name());
		m_avi_target = m_targets->create_target("avibuffer", bgfx::TextureFormat::BGRA8, s_width[0], s_height[0], 1, 1, TARGET_STYLE_CUSTOM, false, true, 1, 0);
		m_avi_texture = bgfx::createTexture2D(s_width[0], s_height[0], false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);

		if (m_avi_view == nullptr)
		{
			m_avi_view = new bgfx_ortho_view(this, 10, m_avi_target, m_seen_views);
		}
	}
}

//============================================================
//  drawsdl_xy_to_render_target
//============================================================

#ifdef OSD_SDL
int renderer_bgfx::xy_to_render_target(int x, int y, int *xt, int *yt)
{
	*xt = x;
	*yt = y;
	if (*xt<0 || *xt >= m_dimensions.width())
		return 0;
	if (*yt<0 || *yt >= m_dimensions.height())
		return 0;
	return 1;
}
#endif

//============================================================
//  drawbgfx_window_draw
//============================================================

bgfx::VertexLayout ScreenVertex::ms_decl;
bgfx::VertexLayout AnalyticLineVertex::ms_decl;

void renderer_bgfx::put_packed_quad(render_primitive *prim, uint32_t hash, ScreenVertex* vertices)
{
	rectangle_packer::packed_rectangle& rect = m_hash_to_entry[hash];
	auto size = float(CACHE_SIZE);
	float u0 = (float(rect.x()) + 0.5f) / size;
	float v0 = (float(rect.y()) + 0.5f) / size;
	float u1 = u0 + (float(rect.width()) - 1.0f) / size;
	float v1 = v0 + (float(rect.height()) - 1.0f) / size;
	uint32_t rgba = u32Color(prim->color.r * 255, prim->color.g * 255, prim->color.b * 255, prim->color.a * 255);

	float x[4] = { prim->bounds.x0, prim->bounds.x1, prim->bounds.x0, prim->bounds.x1 };
	float y[4] = { prim->bounds.y0, prim->bounds.y0, prim->bounds.y1, prim->bounds.y1 };
	float u[4] = { u0, u1, u0, u1 };
	float v[4] = { v0, v0, v1, v1 };

	if (PRIMFLAG_GET_TEXORIENT(prim->flags) & ORIENTATION_SWAP_XY)
	{
		std::swap(u[1], u[2]);
		std::swap(v[1], v[2]);
	}

	if (PRIMFLAG_GET_TEXORIENT(prim->flags) & ORIENTATION_FLIP_X)
	{
		std::swap(u[0], u[1]);
		std::swap(v[0], v[1]);
		std::swap(u[2], u[3]);
		std::swap(v[2], v[3]);
	}

	if (PRIMFLAG_GET_TEXORIENT(prim->flags) & ORIENTATION_FLIP_Y)
	{
		std::swap(u[0], u[2]);
		std::swap(v[0], v[2]);
		std::swap(u[1], u[3]);
		std::swap(v[1], v[3]);
	}

	vertex(&vertices[0], x[0], y[0], 0, rgba, u[0], v[0]);
	vertex(&vertices[1], x[1], y[1], 0, rgba, u[1], v[1]);
	vertex(&vertices[2], x[3], y[3], 0, rgba, u[3], v[3]);
	vertex(&vertices[3], x[3], y[3], 0, rgba, u[3], v[3]);
	vertex(&vertices[4], x[2], y[2], 0, rgba, u[2], v[2]);
	vertex(&vertices[5], x[0], y[0], 0, rgba, u[0], v[0]);
}

void renderer_bgfx::vertex(ScreenVertex* vertex, float x, float y, float z, uint32_t rgba, float u, float v)
{
	vertex->m_x = x;
	vertex->m_y = y;
	vertex->m_z = z;
	vertex->m_rgba = rgba;
	vertex->m_u = u;
	vertex->m_v = v;
}

void renderer_bgfx::render_post_screen_quad(int view, render_primitive* prim, bgfx::TransientVertexBuffer* buffer, int32_t screen, int window_index)
{
	// allocate_buffer leaves data==nullptr when the transient vertex pool is exhausted (e.g. a large
	// beam-window vector draw consumed it). Bail rather than write/submit through a null buffer.
	if (buffer == nullptr || buffer->data == nullptr)
		return;
	auto* vertices = reinterpret_cast<ScreenVertex*>(buffer->data);

	float x[4] = { prim->bounds.x0, prim->bounds.x1, prim->bounds.x0, prim->bounds.x1 };
	float y[4] = { prim->bounds.y0, prim->bounds.y0, prim->bounds.y1, prim->bounds.y1 };
	float u[4] = { prim->texcoords.tl.u, prim->texcoords.tr.u, prim->texcoords.bl.u, prim->texcoords.br.u };
	float v[4] = { prim->texcoords.tl.v, prim->texcoords.tr.v, prim->texcoords.bl.v, prim->texcoords.br.v };

	vertex(&vertices[0], x[0], y[0], 0, 0xffffffff, u[0], v[0]);
	vertex(&vertices[1], x[1], y[1], 0, 0xffffffff, u[1], v[1]);
	vertex(&vertices[2], x[3], y[3], 0, 0xffffffff, u[3], v[3]);
	vertex(&vertices[3], x[3], y[3], 0, 0xffffffff, u[3], v[3]);
	vertex(&vertices[4], x[2], y[2], 0, 0xffffffff, u[2], v[2]);
	vertex(&vertices[5], x[0], y[0], 0, 0xffffffff, u[0], v[0]);

	uint32_t texture_flags = 0U;
	if (!PRIMFLAG_GET_TEXWRAP(prim->flags))
		texture_flags |= BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
	if (video_config.filter == 0)
		texture_flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;

	uint32_t blend = PRIMFLAG_GET_BLENDMODE(prim->flags);
	bgfx::setVertexBuffer(0,buffer);
	bgfx::setTexture(0, m_screen_effect[blend]->uniform("s_tex")->handle(), m_targets->target(screen, "output")->texture(), texture_flags);

	bgfx_uniform* inv_view_dims = m_screen_effect[blend]->uniform("u_inv_view_dims");
	if (inv_view_dims)
	{
		float values[2] = { -1.0f / s_width[window_index], 1.0f / s_height[window_index] };
		inv_view_dims->set(values, sizeof(float) * 2);
		inv_view_dims->upload();
	}

	m_screen_effect[blend]->submit(m_ortho_view->get_index());
}

void renderer_bgfx::render_avi_quad()
{
	const auto view = uint16_t(s_current_view);
	const auto w = uint16_t(s_width[0]);
	const auto h = uint16_t(s_height[0]);

	if (m_vec_hdr_chain)
	{
		// HDR chain: the present pass already encoded the finished frame into m_avi_target
		// (draw() redirects present_fb there while recording), so this view is the display
		// copy avi target -> window backbuffer. update_recording()'s blit is keyed to this
		// same view, which sorts after the present view, so it reads the fresh frame.
		bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
		if (m_framebuffer != nullptr)
			fb = m_framebuffer->target();
		bgfx::setViewFrameBuffer(view, fb);
		bgfx::setViewRect(view, 0, 0, w, h);
		// opaque full-screen quad; the frame-start clear view already handled the backbuffer
		bgfx::setViewClear(view, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
		bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
		float proj[16];
		bx::mtxOrtho(proj, 0.0f, float(w), float(h), 0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
		bgfx::setViewTransform(view, nullptr, proj);
	}
	else
	{
		m_avi_view->set_index(s_current_view);
		m_avi_view->setup();

		bgfx::setViewRect(s_current_view, 0, 0, s_width[0], s_height[0]);
		bgfx::setViewClear(s_current_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
	}

	bgfx::TransientVertexBuffer buffer;
	bgfx::allocTransientVertexBuffer(&buffer, 6, ScreenVertex::ms_decl);
	auto* vertices = reinterpret_cast<ScreenVertex*>(buffer.data);

	float x[4] = { 0.0f, float(s_width[0]), 0.0f, float(s_width[0]) };
	float y[4] = { 0.0f, 0.0f, float(s_height[0]), float(s_height[0]) };
	float u[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
	float v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	uint32_t rgba = 0xffffffff;

	vertex(&vertices[0], x[0], y[0], 0, rgba, u[0], v[0]);
	vertex(&vertices[1], x[1], y[1], 0, rgba, u[1], v[1]);
	vertex(&vertices[2], x[3], y[3], 0, rgba, u[3], v[3]);
	vertex(&vertices[3], x[3], y[3], 0, rgba, u[3], v[3]);
	vertex(&vertices[4], x[2], y[2], 0, rgba, u[2], v[2]);
	vertex(&vertices[5], x[0], y[0], 0, rgba, u[0], v[0]);

	bgfx::setVertexBuffer(0,&buffer);
	bgfx::setTexture(0, m_gui_effect[PRIMFLAG_GET_BLENDMODE(BLENDMODE_NONE)]->uniform("s_tex")->handle(), m_avi_target->texture());

	bgfx_effect* effect = m_gui_effect[PRIMFLAG_GET_BLENDMODE(BLENDMODE_NONE)];
	bgfx_uniform* inv_view_dims = effect->uniform("u_inv_view_dims");
	if (inv_view_dims)
	{
		float values[2] = { -1.0f / s_width[0], 1.0f / s_height[0] };
		inv_view_dims->set(values, sizeof(float) * 2);
		inv_view_dims->upload();
	}

	effect->submit(s_current_view);
	s_current_view++;
}

void renderer_bgfx::render_textured_quad(render_primitive* prim, bgfx::TransientVertexBuffer* buffer, int window_index)
{
	// allocate_buffer leaves data==nullptr when the transient vertex pool is exhausted (e.g. a large
	// beam-window vector draw consumed it). Bail rather than write/submit through a null buffer.
	if (buffer == nullptr || buffer->data == nullptr)
		return;
	auto* vertices = reinterpret_cast<ScreenVertex*>(buffer->data);
	uint32_t rgba = u32Color(prim->color.r * 255, prim->color.g * 255, prim->color.b * 255, prim->color.a * 255);

	float x[4] = { prim->bounds.x0, prim->bounds.x1, prim->bounds.x0, prim->bounds.x1 };
	float y[4] = { prim->bounds.y0, prim->bounds.y0, prim->bounds.y1, prim->bounds.y1 };
	float u[4] = { prim->texcoords.tl.u, prim->texcoords.tr.u, prim->texcoords.bl.u, prim->texcoords.br.u };
	float v[4] = { prim->texcoords.tl.v, prim->texcoords.tr.v, prim->texcoords.bl.v, prim->texcoords.br.v };

	vertex(&vertices[0], x[0], y[0], 0, rgba, u[0], v[0]);
	vertex(&vertices[1], x[1], y[1], 0, rgba, u[1], v[1]);
	vertex(&vertices[2], x[3], y[3], 0, rgba, u[3], v[3]);
	vertex(&vertices[3], x[3], y[3], 0, rgba, u[3], v[3]);
	vertex(&vertices[4], x[2], y[2], 0, rgba, u[2], v[2]);
	vertex(&vertices[5], x[0], y[0], 0, rgba, u[0], v[0]);

	uint32_t texture_flags = 0U;
	if (!PRIMFLAG_GET_TEXWRAP(prim->flags))
		texture_flags |= BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
	if (!PRIMFLAG_GET_ANTIALIAS(prim->flags))
		texture_flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;

	const bool is_screen = PRIMFLAG_GET_SCREENTEX(prim->flags);
	uint16_t tex_width(prim->texture.width);
	uint16_t tex_height(prim->texture.height);

	bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
	if (is_screen)
	{
		const bgfx::Memory* mem = bgfx_util::mame_texture_data_to_bgra32(prim->flags & PRIMFLAG_TEXFORMAT_MASK
			, tex_width, tex_height, prim->texture.rowpixels, prim->texture.palette, prim->texture.base);
		texture = bgfx::createTexture2D(tex_width, tex_height, false, 1, bgfx::TextureFormat::BGRA8, texture_flags, mem);
	}
	else
	{
		texture = m_textures->create_or_update_mame_texture(prim->flags & PRIMFLAG_TEXFORMAT_MASK
			, tex_width, prim->texture.width_margin, tex_height, prim->texture.rowpixels, prim->texture.palette, prim->texture.base, prim->texture.seqid
			, texture_flags, prim->texture.unique_id, prim->texture.old_id);
	}

	// HDR: artwork (non-screen) quads draw into the linear work target with the HDR gui effects
	// (linearize + nits scale + native blend), so MAME's blend modes compose physically.
	bgfx_effect** effects = is_screen ? m_screen_effect : ((m_vec_hdr_chain && !is_screen) ? m_hdr_gui_effect : m_gui_effect);
	bgfx_effect* effect = effects[PRIMFLAG_GET_BLENDMODE(prim->flags)];

	bgfx::setVertexBuffer(0,buffer);
	// Fallback if the source texture is invalid (e.g. atlas not ready on the first frame): leaving the
	// sampler unbound is undefined on some backends and a fatal validation error on Metal.
	const bgfx::TextureHandle quad_tex = (bgfx::isValid(texture) || m_chains == nullptr)
			? texture : m_chains->textures().dummy_handle();
	bgfx::setTexture(0, effect->uniform("s_tex")->handle(), quad_tex);

	bgfx_uniform* inv_view_dims = effect->uniform("u_inv_view_dims");
	if (inv_view_dims)
	{
		float values[2] = { -1.0f / s_width[window_index], 1.0f / s_height[window_index] };
		inv_view_dims->set(values, sizeof(float) * 2);
		inv_view_dims->upload();
	}

	effect->submit(m_ortho_view->get_index());

	if (is_screen)
	{
		bgfx::destroy(texture);
	}
}

#define MAX_TEMP_COORDS 100

void renderer_bgfx::put_polygon(const float* coords, uint32_t num_coords, float r, uint32_t rgba, ScreenVertex* vertex)
{
	float tempCoords[MAX_TEMP_COORDS * 3];
	float tempNormals[MAX_TEMP_COORDS * 2];

	rectangle_packer::packed_rectangle& rect = m_hash_to_entry[WHITE_HASH];
	float u0 = float(rect.x()) / float(CACHE_SIZE);
	float v0 = float(rect.y()) / float(CACHE_SIZE);

	num_coords = num_coords < MAX_TEMP_COORDS ? num_coords : MAX_TEMP_COORDS;

	for (uint32_t ii = 0, jj = num_coords - 1; ii < num_coords; jj = ii++)
	{
		const float* v0 = &coords[jj * 3];
		const float* v1 = &coords[ii * 3];
		float dx = v1[0] - v0[0];
		float dy = v1[1] - v0[1];
		float d = sqrtf(dx * dx + dy * dy);
		if (d > 0)
		{
			d = 1.0f / d;
			dx *= d;
			dy *= d;
		}

		tempNormals[jj * 2 + 0] = dy;
		tempNormals[jj * 2 + 1] = -dx;
	}

	for (uint32_t ii = 0, jj = num_coords - 1; ii < num_coords; jj = ii++)
	{
		float dlx0 = tempNormals[jj * 2 + 0];
		float dly0 = tempNormals[jj * 2 + 1];
		float dlx1 = tempNormals[ii * 2 + 0];
		float dly1 = tempNormals[ii * 2 + 1];
		float dmx = (dlx0 + dlx1) * 0.5f;
		float dmy = (dly0 + dly1) * 0.5f;
		float dmr2 = dmx * dmx + dmy * dmy;
		if (dmr2 > 0.000001f)
		{
			float scale = 1.0f / dmr2;
			if (scale > 10.0f)
			{
				scale = 10.0f;
			}

			dmx *= scale;
			dmy *= scale;
		}

		tempCoords[ii * 3 + 0] = coords[ii * 3 + 0] + dmx * r;
		tempCoords[ii * 3 + 1] = coords[ii * 3 + 1] + dmy * r;
		tempCoords[ii * 3 + 2] = coords[ii * 3 + 2];
	}

	int vertIndex = 0;
	uint32_t trans = rgba & 0x00ffffff;
	for (uint32_t ii = 0, jj = num_coords - 1; ii < num_coords; jj = ii++)
	{
		vertex[vertIndex].m_x = coords[ii * 3 + 0];
		vertex[vertIndex].m_y = coords[ii * 3 + 1];
		vertex[vertIndex].m_z = coords[ii * 3 + 2];
		vertex[vertIndex].m_rgba = rgba;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = coords[jj * 3 + 0];
		vertex[vertIndex].m_y = coords[jj * 3 + 1];
		vertex[vertIndex].m_z = coords[jj * 3 + 2];
		vertex[vertIndex].m_rgba = rgba;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = tempCoords[jj * 3 + 0];
		vertex[vertIndex].m_y = tempCoords[jj * 3 + 1];
		vertex[vertIndex].m_z = tempCoords[jj * 3 + 2];
		vertex[vertIndex].m_rgba = trans;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = tempCoords[jj * 3 + 0];
		vertex[vertIndex].m_y = tempCoords[jj * 3 + 1];
		vertex[vertIndex].m_z = tempCoords[jj * 3 + 2];
		vertex[vertIndex].m_rgba = trans;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = tempCoords[ii * 3 + 0];
		vertex[vertIndex].m_y = tempCoords[ii * 3 + 1];
		vertex[vertIndex].m_z = tempCoords[ii * 3 + 2];
		vertex[vertIndex].m_rgba = trans;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = coords[ii * 3 + 0];
		vertex[vertIndex].m_y = coords[ii * 3 + 1];
		vertex[vertIndex].m_z = coords[ii * 3 + 2];
		vertex[vertIndex].m_rgba = rgba;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;
	}

	for (uint32_t ii = 2; ii < num_coords; ++ii)
	{
		vertex[vertIndex].m_x = coords[0];
		vertex[vertIndex].m_y = coords[1];
		vertex[vertIndex].m_z = coords[2];
		vertex[vertIndex].m_rgba = rgba;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = coords[(ii - 1) * 3 + 0];
		vertex[vertIndex].m_y = coords[(ii - 1) * 3 + 1];
		vertex[vertIndex].m_z = coords[(ii - 1) * 3 + 2];
		vertex[vertIndex].m_rgba = rgba;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;

		vertex[vertIndex].m_x = coords[ii * 3 + 0];
		vertex[vertIndex].m_y = coords[ii * 3 + 1];
		vertex[vertIndex].m_z = coords[ii * 3 + 2];
		vertex[vertIndex].m_rgba = rgba;
		vertex[vertIndex].m_u = u0;
		vertex[vertIndex].m_v = v0;
		vertIndex++;
	}
}

void renderer_bgfx::put_packed_line(render_primitive *prim, ScreenVertex* vertex)
{
	float width = prim->width < 0.5f ? 0.5f : prim->width;
	float x0 = prim->bounds.x0;
	float y0 = prim->bounds.y0;
	float x1 = prim->bounds.x1;
	float y1 = prim->bounds.y1;
	uint32_t rgba = u32Color(prim->color.r * 255, prim->color.g * 255, prim->color.b * 255, prim->color.a * 255);

	put_line(x0, y0, x1, y1, width, rgba, vertex, 1.0f);
}

void renderer_bgfx::put_line(float x0, float y0, float x1, float y1, float r, uint32_t rgba, ScreenVertex* vertex, float fth)
{
	float dx = x1 - x0;
	float dy = y1 - y0;
	float d = sqrtf(dx * dx + dy * dy);
	if (d > 0.0001f)
	{
		d = 1.0f / d;
		dx *= d;
		dy *= d;
	}

	// create diamond shape for points
	else
	{
		// set distance to unit vector length (1,1)
		dx = dy = 0.70710678f;
	}

	float nx = dy;
	float ny = -dx;
	float verts[4 * 3];
	r -= fth;
	r *= 0.5f;
	if (r < 0.01f)
	{
		r = 0.01f;
	}

	dx *= r;
	dy *= r;
	nx *= r;
	ny *= r;

	verts[0] = x0 - dx - nx;
	verts[1] = y0 - dy - ny;
	verts[2] = 0;

	verts[3] = x0 - dx + nx;
	verts[4] = y0 - dy + ny;
	verts[5] = 0;

	verts[6] = x1 + dx + nx;
	verts[7] = y1 + dy + ny;
	verts[8] = 0;

	verts[9] = x1 + dx - nx;
	verts[10] = y1 + dy - ny;
	verts[11] = 0;

	put_polygon(verts, 4, fth, rgba, vertex);
}

uint32_t renderer_bgfx::u32Color(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255)
{
	return (a << 24) | (b << 16) | (g << 8) | r;
}

// Analytic-AA vector line drawing.
// Draw a line as a single quad from 6 vertices. The vertex UV spans 0..1 across the line width,
// and fs_vector_line.sc computes the parabolic fade `1 - (2|v-0.5|)^2`.
// -> Because GPU interpolation is linear, the same width profile results regardless of angle.
// Rounded line caps - add a fan of N triangles at each end (half-circle approximation).
// 6 (line body) + 2 * N * 3 = 6 + 6N vertices/line. N=8 gives 54 vertices.
static constexpr int   LINE_CAP_SEGMENTS      = 8;
static constexpr int   LINE_VERTICES_PER_LINE = 6 + 2 * LINE_CAP_SEGMENTS * 3;
static constexpr float LINE_CAP_SIZE_PX       = 2.0f;  // cap radius (px at a 1920px-wide window)
static constexpr float LINE_POINT_THRESHOLD   = 2.0f;  // segments shorter than this are drawn as points

// Analytic gaussian line integral: emit one expanded quad (6 vertices) carrying the
// line-local coordinates; fs_vector_line_analytic evaluates the swept-spot exposure in
// closed form, so AA, the cross profile and the end roll-off all come from the math.
// Integrate one vector segment (S -> E over draw_secs) through the X/Y deflection amplifiers, modelled
// as independent second-order systems (natural frequency wn from deflection_settle, damping zeta).
// The beam state (position + velocity) is carried continuously from the previous segment, so a sharp
// direction change overshoots into a corner "hook" and a post-jump start curves in. Returns DEFL_NOUT
// (segment count); outx/outy receive DEFL_NOUT+1 trajectory points, blended toward the straight chord
// by deflection_dynamics so strength 0 renders an exact straight line. Updates the beam state.
int renderer_bgfx::simulate_deflection(float sx, float sy, float ex, float ey, double draw_secs, float *outx, float *outy)
{
	const int   N        = DEFL_NOUT;
	const float strength = std::clamp(m_vs.deflection_dynamics, 0.0f, 1.0f);
	const float settle_us = std::max(0.1f, m_vs.deflection_settle);
	const float zeta     = std::clamp(m_vs.deflection_damping, 0.05f, 2.0f);
	const float res      = m_vec_res_w / 1920.0f;

	double T = draw_secs;
	if (!(T > 1e-9)) T = 1e-6;   // untimed / degenerate: nominal time (settles to straight)
	const float wn = 1.0f / (settle_us * 1e-6f);

	const float vcx = float((ex - sx) / T);   // commanded (steady-state) velocity
	const float vcy = float((ey - sy) / T);

	// Entry state: continuous when the beam is already at the segment start (a shared vertex - the
	// hook comes from the carried velocity); otherwise a new stroke / post-jump start, taken as settled.
	float px, py, vx, vy;
	const float rthresh = 4.0f * res;
	if (m_beam_valid && fabsf(m_beam_px - sx) < rthresh && fabsf(m_beam_py - sy) < rthresh)
	{
		px = m_beam_px; py = m_beam_py; vx = m_beam_vx; vy = m_beam_vy;
	}
	else
	{
		px = sx; py = sy; vx = vcx; vy = vcy;
	}

	const double dt_max = std::min(T / N, double(0.3f / wn));
	int Nint = int(std::ceil(T / std::max(dt_max, 1e-12)));
	Nint = std::clamp(Nint, N, 256);
	const double dt = T / Nint;

	outx[0] = px; outy[0] = py;
	int next_out = 1;
	for (int k = 1; k <= Nint; k++)
	{
		const float frac = float((k * dt) / T);
		const float cmdx = sx + (ex - sx) * frac;
		const float cmdy = sy + (ey - sy) * frac;
		const float ax = wn * wn * (cmdx - px) - 2.0f * zeta * wn * (vx - vcx);
		const float ay = wn * wn * (cmdy - py) - 2.0f * zeta * wn * (vy - vcy);
		vx += ax * float(dt); vy += ay * float(dt);
		px += vx * float(dt); py += vy * float(dt);
		while (next_out <= N && k >= int(llround(double(next_out) * Nint / N)))
		{
			outx[next_out] = px; outy[next_out] = py;
			++next_out;
		}
	}
	while (next_out <= N) { outx[next_out] = px; outy[next_out] = py; ++next_out; }

	m_beam_px = px; m_beam_py = py; m_beam_vx = vx; m_beam_vy = vy; m_beam_valid = true;

	for (int i = 0; i <= N; i++)
	{
		const float frac = float(i) / float(N);
		const float strx = sx + (ex - sx) * frac;
		const float stry = sy + (ey - sy) * frac;
		outx[i] = strx + (outx[i] - strx) * strength;
		outy[i] = stry + (outy[i] - stry) * strength;
	}
	return N;
}

// Symmetric S-curve / sigmoid mapping of a 0..1 value about a pivot c. g>0 = sigmoid (push values away
// from c toward 0/1 -> sharper, snappier transition); g<0 = inverse-S (ease around c); g=0 = identity.
// k = 2^g. Shared by the brightness/width curves and the overload_gain shaping.
static float vec_scurve(float x, float g, float c)
{
	if (g == 0.0f) return x;
	x = std::clamp(x, 0.0f, 1.0f);
	c = std::clamp(c, 1e-3f, 1.0f - 1e-3f);
	const float k = powf(2.0f, g);
	return (x < c) ? c * powf(x / c, k)
				   : c + (1.0f - c) * (1.0f - powf(1.0f - (x - c) / (1.0f - c), k));
}

// Slider-cache field table: chain slider name -> vec_slider_cache member -> code default.
// Names are resolved to bgfx_slider pointers once per chain switch (rebuild below); the
// per-frame refresh is then ~85 plain float loads, cheap even at unthrottled frame rates.
namespace {
struct vec_slider_def { const char *name; float renderer_bgfx::vec_slider_cache::*field; float def; };
const vec_slider_def VEC_SLIDER_DEFS[] = {
	{ "analytic_glow", &renderer_bgfx::vec_slider_cache::analytic_glow, 0.0f },
	{ "analytic_glow_width", &renderer_bgfx::vec_slider_cache::analytic_glow_width, 8.0f },
	{ "beam_noise", &renderer_bgfx::vec_slider_cache::beam_noise, 0.0f },
	{ "beam_width_max", &renderer_bgfx::vec_slider_cache::beam_width_max, 1.5f },
	{ "beam_width_min", &renderer_bgfx::vec_slider_cache::beam_width_min, 1.0f },
	{ "beam_width_over_scale", &renderer_bgfx::vec_slider_cache::beam_width_over_scale, -1.0f },
	{ "beam_width_overmax", &renderer_bgfx::vec_slider_cache::beam_width_overmax, 4.0f },
	{ "bright_curve", &renderer_bgfx::vec_slider_cache::bright_curve, 1.0f },
	{ "bright_normal_cap", &renderer_bgfx::vec_slider_cache::bright_normal_cap, 1.0f },
	{ "bright_sigmoid", &renderer_bgfx::vec_slider_cache::bright_sigmoid, 0.0f },
	{ "bright_sigmoid_center", &renderer_bgfx::vec_slider_cache::bright_sigmoid_center, 0.5f },
	{ "bright_threshold", &renderer_bgfx::vec_slider_cache::bright_threshold, 0.0f },
	{ "core_flat", &renderer_bgfx::vec_slider_cache::core_flat, 0.0f },
	{ "convergence_bloom_gain", &renderer_bgfx::vec_slider_cache::convergence_bloom_gain, 0.0f },
	{ "convergence_bloom_falloff", &renderer_bgfx::vec_slider_cache::convergence_bloom_falloff, 96.0f },
	{ "convergence_bloom_knee", &renderer_bgfx::vec_slider_cache::convergence_bloom_knee, 8.0f },
	{ "convergence_bloom_min_support", &renderer_bgfx::vec_slider_cache::convergence_bloom_min_support, 110.0f },
	{ "convergence_bloom_source_radius", &renderer_bgfx::vec_slider_cache::convergence_bloom_source_radius, 0.0f },
	{ "convergence_bloom_threshold", &renderer_bgfx::vec_slider_cache::convergence_bloom_threshold, 8.0f },
	{ "convergence_global_gain", &renderer_bgfx::vec_slider_cache::convergence_global_gain, 0.0f },
	{ "convergence_global_coverage", &renderer_bgfx::vec_slider_cache::convergence_global_coverage, 0.55f },
	{ "deflection_damping", &renderer_bgfx::vec_slider_cache::deflection_damping, 0.5f },
	{ "deflection_dynamics", &renderer_bgfx::vec_slider_cache::deflection_dynamics, 0.0f },
	{ "deflection_settle", &renderer_bgfx::vec_slider_cache::deflection_settle, 5.0f },
	{ "dot_no_persist_dwell", &renderer_bgfx::vec_slider_cache::dot_no_persist_dwell, 0.0f },
	{ "edge_defocus", &renderer_bgfx::vec_slider_cache::edge_defocus, 0.0f },
	{ "edge_defocus_curve", &renderer_bgfx::vec_slider_cache::edge_defocus_curve, 2.0f },
	{ "energy_curve", &renderer_bgfx::vec_slider_cache::energy_curve, 1.0f },
	{ "energy_dot_curve", &renderer_bgfx::vec_slider_cache::energy_dot_curve, 1.6f },
	{ "energy_dot_max", &renderer_bgfx::vec_slider_cache::energy_dot_max, 3.2f },
	{ "energy_dot_ref", &renderer_bgfx::vec_slider_cache::energy_dot_ref, 30.0f },
	{ "energy_dwell_cap", &renderer_bgfx::vec_slider_cache::energy_dwell_cap, 16.0f },
	{ "energy_infl", &renderer_bgfx::vec_slider_cache::energy_infl, 0.6f },
	{ "energy_jitter", &renderer_bgfx::vec_slider_cache::energy_jitter, 0.0f },
	{ "energy_jitter_base", &renderer_bgfx::vec_slider_cache::energy_jitter_base, 0.0f },
	{ "energy_jitter_hz", &renderer_bgfx::vec_slider_cache::energy_jitter_hz, 15.0f },
	{ "energy_jitter_onset", &renderer_bgfx::vec_slider_cache::energy_jitter_onset, 0.8f },
	{ "energy_jitter_ramp", &renderer_bgfx::vec_slider_cache::energy_jitter_ramp, 0.5f },
	{ "energy_line_max", &renderer_bgfx::vec_slider_cache::energy_line_max, 4.0f },
	{ "energy_model", &renderer_bgfx::vec_slider_cache::energy_model, 0.0f },
	{ "energy_obj_knee", &renderer_bgfx::vec_slider_cache::energy_obj_knee, 0.75f },
	{ "energy_obj_lift", &renderer_bgfx::vec_slider_cache::energy_obj_lift, 0.0f },
	{ "energy_obj_max", &renderer_bgfx::vec_slider_cache::energy_obj_max, 3.0f },
	{ "energy_obj_sharp", &renderer_bgfx::vec_slider_cache::energy_obj_sharp, 2.0f },
	{ "energy_obj_star", &renderer_bgfx::vec_slider_cache::energy_obj_star, 1.5f },
	{ "energy_speed_norm", &renderer_bgfx::vec_slider_cache::energy_speed_norm, 0.8f },
	{ "energy_stroke_agg", &renderer_bgfx::vec_slider_cache::energy_stroke_agg, 1.0f },
	{ "glow_curve", &renderer_bgfx::vec_slider_cache::glow_curve, 1.0f },
	{ "glow_narrow", &renderer_bgfx::vec_slider_cache::glow_narrow, 0.0f },
	{ "glow_threshold", &renderer_bgfx::vec_slider_cache::glow_threshold, 0.0f },
	{ "hv_droop", &renderer_bgfx::vec_slider_cache::hv_droop, 0.0f },
	{ "intensity_overdrive", &renderer_bgfx::vec_slider_cache::intensity_overdrive, 0.0f },
	{ "intensity_overdrive_curve", &renderer_bgfx::vec_slider_cache::intensity_overdrive_curve, 2.0f },
	{ "line_cap_brightness", &renderer_bgfx::vec_slider_cache::line_cap_brightness, 1.0f },
	{ "line_cap_intensity_curve", &renderer_bgfx::vec_slider_cache::line_cap_intensity_curve, 0.0f },
	{ "line_cap_junction_suppress", &renderer_bgfx::vec_slider_cache::line_cap_junction_suppress, 0.0f },
	{ "line_cap_max_size", &renderer_bgfx::vec_slider_cache::line_cap_max_size, 0.0f },
	{ "line_cap_mode", &renderer_bgfx::vec_slider_cache::line_cap_mode, 0.0f },
	{ "line_cap_min_size", &renderer_bgfx::vec_slider_cache::line_cap_min_size, 0.0f },
	{ "line_cap_size", &renderer_bgfx::vec_slider_cache::line_cap_size, 2.0f },
	{ "line_cap_width", &renderer_bgfx::vec_slider_cache::line_cap_width, 1.5f },
	{ "line_point_threshold", &renderer_bgfx::vec_slider_cache::line_point_threshold, 2.0f },
	{ "linear_color", &renderer_bgfx::vec_slider_cache::linear_color, 0.0f },
	{ "overdrive_core", &renderer_bgfx::vec_slider_cache::overdrive_core, 0.0f },
	{ "overdrive_sat_curve", &renderer_bgfx::vec_slider_cache::overdrive_sat_curve, 1.0f },
	{ "overload_bloom", &renderer_bgfx::vec_slider_cache::overload_bloom, 0.0f },
	{ "overload_dot_gain", &renderer_bgfx::vec_slider_cache::overload_dot_gain, 1.0f },
	{ "overload_gain", &renderer_bgfx::vec_slider_cache::overload_gain, 0.0f },
	{ "overload_gain_center", &renderer_bgfx::vec_slider_cache::overload_gain_center, 0.5f },
	{ "overload_glow_gain", &renderer_bgfx::vec_slider_cache::overload_glow_gain, 0.0f },
	{ "overload_glow_width", &renderer_bgfx::vec_slider_cache::overload_glow_width, 40.0f },
	{ "overload_max", &renderer_bgfx::vec_slider_cache::overload_max, 0.0f },
	{ "overload_ramp", &renderer_bgfx::vec_slider_cache::overload_ramp, 0.0f },
	{ "overload_threshold", &renderer_bgfx::vec_slider_cache::overload_threshold, 1.0f },
	{ "overload_width_add", &renderer_bgfx::vec_slider_cache::overload_width_add, -1.0f },
	{ "overload_width_center", &renderer_bgfx::vec_slider_cache::overload_width_center, 0.65f },
	{ "overload_width_steepness", &renderer_bgfx::vec_slider_cache::overload_width_steepness, 10.0f },
	{ "phosphor_overdrive", &renderer_bgfx::vec_slider_cache::phosphor_overdrive, 0.0f },
	{ "point_width_scale", &renderer_bgfx::vec_slider_cache::point_width_scale, 1.0f },
	{ "point_brightness_scale", &renderer_bgfx::vec_slider_cache::point_brightness_scale, 1.0f },
	{ "ray_angle", &renderer_bgfx::vec_slider_cache::ray_angle, 15.0f },
	{ "ray_count_rand", &renderer_bgfx::vec_slider_cache::ray_count_rand, 0.0f },
	{ "ray_gain", &renderer_bgfx::vec_slider_cache::ray_gain, 0.0f },
	{ "ray_length", &renderer_bgfx::vec_slider_cache::ray_length, 60.0f },
	{ "ray_length_rand", &renderer_bgfx::vec_slider_cache::ray_length_rand, 0.0f },
	{ "ray_var", &renderer_bgfx::vec_slider_cache::ray_var, 0.6f },
	{ "ray_width", &renderer_bgfx::vec_slider_cache::ray_width, 1.2f },
	{ "ring_fill", &renderer_bgfx::vec_slider_cache::ring_fill, 0.0f },
	{ "ring_gain", &renderer_bgfx::vec_slider_cache::ring_gain, 0.0f },
	{ "halation_gain", &renderer_bgfx::vec_slider_cache::halation_gain, 1.0f },
	{ "ring_min_dwell", &renderer_bgfx::vec_slider_cache::ring_min_dwell, 0.0f },
	{ "ring_over_gain", &renderer_bgfx::vec_slider_cache::ring_over_gain, 0.0f },
	{ "ring_radius", &renderer_bgfx::vec_slider_cache::ring_radius, 24.0f },
	{ "ring_threshold", &renderer_bgfx::vec_slider_cache::ring_threshold, 0.0f },
	{ "ring_width", &renderer_bgfx::vec_slider_cache::ring_width, 3.0f },
	{ "vector_linearity_x", &renderer_bgfx::vec_slider_cache::vector_linearity_x, 1.0f },
	{ "vector_linearity_y", &renderer_bgfx::vec_slider_cache::vector_linearity_y, 1.0f },
	{ "width_curve", &renderer_bgfx::vec_slider_cache::width_curve, 1.0f },
	{ "width_knee", &renderer_bgfx::vec_slider_cache::width_knee, 0.3f },
	{ "width_over_curve", &renderer_bgfx::vec_slider_cache::width_over_curve, 1.0f },
	{ "width_sigmoid", &renderer_bgfx::vec_slider_cache::width_sigmoid, 0.0f },
	{ "width_sigmoid_center", &renderer_bgfx::vec_slider_cache::width_sigmoid_center, 0.5f },
	{ "z_rise_tau", &renderer_bgfx::vec_slider_cache::z_rise_tau, 0.0f },
};
} // anonymous namespace

// Re-resolve the slider-name -> slider-pointer map for the active screen-0 chain. Absent
// sliders leave their cache field at the code default (the same fallback the old per-vector
// slider_value() reads used). float sliders register under name + "0", vec2/color components
// under name + component index (see slider_reader).
void renderer_bgfx::rebuild_vec_slider_map()
{
	m_vs = vec_slider_cache();   // reset every field to its code default
	m_vs_map.clear();
	m_vs_knee0 = m_vs_knee1 = nullptr;
	m_vs_ovcol[0] = m_vs_ovcol[1] = m_vs_ovcol[2] = nullptr;
	if (m_vs_src_chain == nullptr)
		return;
	auto find = [this](const std::string &suffixed) -> bgfx_slider* {
		for (bgfx_slider *slider : m_vs_src_chain->sliders())
			if (slider->name() == suffixed)
				return slider;
		return nullptr;
	};
	for (const vec_slider_def &def : VEC_SLIDER_DEFS)
	{
		if (bgfx_slider *slider = find(std::string(def.name) + "0"))
			m_vs_map.emplace_back(def.field, slider);
	}
	m_vs_knee0 = find("overdrive_knee0");
	m_vs_knee1 = find("overdrive_knee1");
	for (int c = 0; c < 3; c++)
		m_vs_ovcol[c] = find("overdrive_color" + std::to_string(c));
}

// Refresh the per-frame slider cache (see vec_slider_cache in the header). Called once per
// draw(); slider edits apply on the next frame, a chain switch triggers a map rebuild.
void renderer_bgfx::refresh_vec_slider_cache()
{
	bgfx_chain *chain = (m_chains != nullptr) ? m_chains->screen_chain(0) : nullptr;
	if (chain != m_vs_src_chain)
	{
		m_vs_src_chain = chain;
		rebuild_vec_slider_map();
	}
	for (const auto &entry : m_vs_map)
		m_vs.*entry.first = entry.second->value();
	m_vs.overdrive_knee = (m_vs_knee0 != nullptr) ? m_vs_knee0->value() : 0.6f;
	// Ceiling component: absent on a chain still carrying a plain float knee - fall back to
	// knee + eps, reproducing that chain's original hard-step (division-guard-only) behaviour.
	m_vs.overdrive_ceil = std::max((m_vs_knee1 != nullptr) ? m_vs_knee1->value() : m_vs.overdrive_knee + 1e-4f, m_vs.overdrive_knee + 1e-4f);
	for (int c = 0; c < 3; c++)
		m_vs.overdrive_color[c] = (m_vs_ovcol[c] != nullptr) ? m_vs_ovcol[c]->value() : 1.0f;
}

// Unified beam-energy model for sources that do not supply beam_energy (DVG / AVG / Cinematronics):
// derive it renderer-side from the per-segment timestamps, with the same convention as the Vectrex
// driver (0..1 = normal display range, >1 = overdrive from slow sweeps / dwelling dots).
// Lines: density ~ 1/speed through the saturating s = x^g/(x^g+1) (speed in screen-widths per ms,
// so it is resolution-independent). Points: dwell time through the same curve family.
// stroke_px_per_ms >= 0 overrides the per-segment speed with the WHOLE-STROKE aggregate (see the
// energy_stroke_agg pre-pass in draw()): a curve drawn as many short sub-segments then reads one
// uniform speed instead of per-segment noise (matching the driver model's whole-stroke density).
// energy_model 0 = off (n = display intensity, prior behaviour - chains without the slider unchanged).
float renderer_bgfx::generic_beam_energy(render_primitive *prim, float seg_len, bool as_point, float screen_ref, float stroke_px_per_ms)
{
	const float I = std::clamp(prim->color.a, 0.0f, 1.0f);
	if (m_vs.energy_model <= 0.0f
		|| !(prim->t0 >= 0.0 && prim->t1 > prim->t0) || I <= 0.0f)
		return I;
	const double dt_ms = (prim->t1 - prim->t0) * 1000.0;
	const float infl = std::clamp(m_vs.energy_infl, 0.0f, 1.0f);
	double s, emax;
	if (as_point)
	{
		const double x  = (dt_ms * 1000.0) / std::max(1.0, double(m_vs.energy_dot_ref));   // dwell us / ref us
		const double xg = std::pow(std::max(0.0, x), double(std::max(0.05f, m_vs.energy_dot_curve)));
		s = xg / (xg + 1.0);
		emax = std::max(1.0f, m_vs.energy_dot_max);
	}
	else
	{
		const double v  = (stroke_px_per_ms >= 0.0f)
				? double(stroke_px_per_ms) / std::max(1.0f, screen_ref)                              // aggregate stroke speed
				: (double(seg_len) / std::max(1.0f, screen_ref)) / std::max(1e-6, dt_ms);            // screen-widths per ms
		const double x  = double(std::max(0.01f, m_vs.energy_speed_norm)) / std::max(1e-6, v);
		const double xg = std::pow(std::max(0.0, x), double(std::max(0.05f, m_vs.energy_curve)));
		s = xg / (xg + 1.0);
		emax = std::max(1.0f, m_vs.energy_line_max);
	}
	return float(std::clamp(double(I) * ((1.0 - infl) + infl * s * emax), 0.0, 16.0));
}

// Port of the Vectrex driver's object_boost() (see vectrex_v.cpp): beam_energy *= 1..energy_obj_max
// as intensity rises past energy_obj_knee (sharpness energy_obj_sharp); point-classified (parked-dot)
// primitives get an extra energy_obj_star factor. energy_obj_lift <= 0 = off. Model-derived energy
// only (the caller gates on prim->beam_energy < 0.0f).
float renderer_bgfx::energy_object_lift(float intensity01, bool as_point) const
{
	if (m_vs.energy_obj_lift <= 0.0f)
		return 1.0f;
	const float knee = std::clamp(m_vs.energy_obj_knee, 0.0f, 0.999f);
	const float t = std::clamp((intensity01 - knee) / std::max(1e-3f, 1.0f - knee), 0.0f, 1.0f);
	const float sharp = std::max(0.1f, m_vs.energy_obj_sharp);
	const float lift = (sharp == 1.0f) ? t : powf(t, sharp);
	float b = 1.0f + (std::max(1.0f, m_vs.energy_obj_max) - 1.0f) * lift;
	if (as_point)
		b *= std::max(1.0f, m_vs.energy_obj_star);
	return b;
}

// DAC / integrator position noise: a time-coherent 2D offset (px) for a beam endpoint, modelling the
// analog deflection chain's noise floor. Keyed on the endpoint POSITION so a shared vertex (two
// connected strokes) and a parked dot's two coincident endpoints get the SAME offset - the beam path
// stays joined and a dot stays a dot. The time axis is emulated time quantized to energy_jitter_hz
// steps with smoothstep value-noise between them (bounded speed, freezes on pause). beam_noise 0 = off.
void renderer_bgfx::beam_noise_offset(float x, float y, float &ox, float &oy) const
{
	ox = oy = 0.0f;
	const float amt = m_vs.beam_noise;
	if (amt <= 0.0f)
		return;
	const float hz = std::max(1.0f, m_vs.energy_jitter_hz);   // share the jitter cadence
	const double t = m_vec_time_ms * double(hz) * 0.001;
	const uint32_t step = uint32_t(int64_t(t));
	const float frac = float(t - double(step));
	const float sm = frac * frac * (3.0f - 2.0f * frac);
	auto h = [](uint32_t a) { a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16; return a; };
	const uint32_t seed = h(uint32_t(int32_t(x))) ^ h(uint32_t(int32_t(y)) + 0x9e3779b9U);
	auto nz = [&](uint32_t chan) -> float {
		const uint32_t s = seed ^ h(chan);
		const float a0 = float(h(s ^ h(step))      & 0xffffffu) / float(0x800000) - 1.0f;
		const float a1 = float(h(s ^ h(step + 1u)) & 0xffffffu) / float(0x800000) - 1.0f;
		return a0 + (a1 - a0) * sm;
	};
	ox = amt * nz(0x68f1u);
	oy = amt * nz(0xb5e3u);
}

void renderer_bgfx::put_analytic_line(render_primitive *prim, AnalyticLineVertex *vertex, AnalyticLineVertex *glow_vertex, AnalyticLineVertex *optical_vertex, AnalyticLineVertex *np_vertex, AnalyticLineVertex *ray_vertex, float start_cap, float end_cap, float stroke_px_per_ms, float dwell_scale)
{
	float x0 = prim->bounds.x0, y0 = prim->bounds.y0;
	float x1 = prim->bounds.x1, y1 = prim->bounds.y1;

	// Vector linearity calibration (board "Linear" / X-Y SIZE pots = per-axis integrator gain): draw
	// each vector as the commanded delta x gain (X and Y independent), continuing from where the beam
	// actually ended up. A gain != 1 makes a contiguous stroke grow/shrink and the error accumulate
	// along it, resetting at a jump (a start that does not meet the previous commanded end) or a new
	// frame. 1.0 / 1.0 = exact (off).
	const float lx = m_vs.vector_linearity_x;
	const float ly = m_vs.vector_linearity_y;
	if (lx != 1.0f || ly != 1.0f)
	{
		float sx, sy;
		if (m_lin_valid && fabsf(x0 - m_lin_cmd_ex) < 0.5f && fabsf(y0 - m_lin_cmd_ey) < 0.5f)
		{ sx = m_lin_drawn_ex; sy = m_lin_drawn_ey; }   // contiguous: continue from the drawn end
		else
		{ sx = x0; sy = y0; }                            // jump / first vector: beam at commanded start
		const float ex = sx + (x1 - x0) * lx;
		const float ey = sy + (y1 - y0) * ly;
		m_lin_cmd_ex = x1; m_lin_cmd_ey = y1;            // remember commanded end for the next test
		m_lin_drawn_ex = ex; m_lin_drawn_ey = ey;
		m_lin_valid = true;
		x0 = sx; y0 = sy; x1 = ex; y1 = ey;
	}

	float dx = x1 - x0, dy = y1 - y0;
	const float seg_len = sqrtf(dx * dx + dy * dy);

	const float point_threshold = m_vs.line_point_threshold;
	const bool as_point = (seg_len <= point_threshold);

	// DAC / integrator position noise: jitter each endpoint by a time-coherent, position-keyed offset
	// (analog deflection noise). Applied AFTER the point/line classification (so a dot - whose two
	// endpoints share a position and thus an offset - stays a dot and is not reclassified as a line);
	// the clean dx/dy/seg_len above are kept for the beam direction, width and energy (a sub-pixel
	// offset does not change them meaningfully), only the drawn endpoint positions move.
	if (m_vs.beam_noise > 0.0f)
	{
		float ox0, oy0, ox1, oy1;
		beam_noise_offset(x0, y0, ox0, oy0);
		beam_noise_offset(x1, y1, ox1, oy1);
		x0 += ox0; y0 += oy0; x1 += ox1; y1 += oy1;
	}

	// Unified per-vector transfers. drive = beam_energy when the device supplies it (AVG: Tempest /
	// Star Wars / Major Havoc), else the display intensity (DVG: Asteroids etc.). Brightness and width
	// are two independent clipped power curves: out = clamp((drive-lo)/(hi-lo),0,1)^gamma. Decoupling
	// them lets brightness saturate early while width keeps growing - this subsumes the old overload
	// model (now removed). lo=0 / hi=1 / curve=1 reproduces the plain intensity-linear response.
	// n = the normalized beam energy; n > 1 is the genuine overload that drives the white flare/bloom.
	// (The legacy beam_energy_ref divisor is gone - the unified energy model's energy_speed_norm and
	// overload_threshold cover its role.) When the device supplies NO beam_energy (< 0) the renderer
	// derives it from the per-segment timestamps (unified model, generic_beam_energy); with energy_model
	// off that reduces to the plain display intensity clamp(color.a) = the prior behaviour.
	// Speed-normalisation basis = the vector CONTENT width (same basis as every px magnitude), NOT
	// the window: a portrait game pillarboxed in a wide window would otherwise read its beam speeds
	// ~2x too slow (window-relative lengths) and everything would run hot.
	const float e_screen_ref = (m_vec_res_w > 1.0f) ? m_vec_res_w
			: float(std::max(s_width[window().index()], s_height[window().index()]));
	float n = (prim->beam_energy >= 0.0f) ? prim->beam_energy
										  : generic_beam_energy(prim, seg_len, as_point, e_screen_ref, stroke_px_per_ms);
	// Same-spot dwell cap (energy_dwell_cap pre-pass in draw()): a run of dots deposited on ONE spot
	// piles energy up only to the cap - the later dots' MODEL-DERIVED energy is scaled down by the
	// pre-computed factor. Applies only when the renderer derived n itself (beam_energy < 0); a
	// device-supplied energy (e.g. the Vectrex driver model) already caps its own pile-up.
	if (prim->beam_energy < 0.0f && dwell_scale < 1.0f)
		n *= dwell_scale;
	// Object-type lift (energy_object_lift port of the Vectrex driver's object_boost - see there):
	// model-derived energy only, applied AFTER the dwell cap (matching the driver's own order - a
	// bullet/star lift can push a spot back past what the dwell cap already capped it to).
	// POINTS ONLY: the driver only boosts beam_energy >= 0 content, which in renderer-equivalent
	// terms is the dwelling-dot population (bullets/stars); fast line content (e.g. BIOS text) is
	// beam_energy = -1 there and stays unboosted. Lifting fast lines here amplified their infl floor
	// past the overload threshold, which made the genuinely multiplexed (drawn ~1-in-4-frames, real
	// hardware) BIOS text lines beat visibly. Genuinely slow/bright lines still overload via the line
	// branch's own energy_line_max, so restricting the lift to points loses no legitimate line flare.
	if (prim->beam_energy < 0.0f && as_point)
		n *= energy_object_lift(std::clamp(prim->color.a, 0.0f, 1.0f), true);
	// Z rise-time response (points, any source): the blanking / Z-amp / cathode drive has a finite
	// response, so a dot parked only a brief time does not let the beam current reach full value and
	// deposits less energy than a fully-established beam. Effective factor = 1 - exp(-dt/tau).
	// This is the physical replacement for the retired junction_dot geometric hack: a mid-stroke dot
	// deposited by a brief beam pause is dimmed because it IS a brief dwell, not because it happens to
	// lie on a line - and because the two-regime transfer couples brightness->width, a dimmed dot also
	// reads thinner, reproducing the old width-shrink as a consequence. It applies REGARDLESS of the
	// beam_energy source: every energy model (device-supplied like Star Wars AVG, or the renderer's own
	// generic_beam_energy) assumes the beam reaches full current, so this current-establishment stage
	// is a distinct, additive display effect - it is not double-counting the phosphor dwell (energy =
	// current x time). The junction dots it targets (e.g. Star Wars: dots drawn at the same beam_energy
	// as the lines but a ~0.08us dwell vs the lines' ~5us) are dimmed hard while the lines are untouched.
	// z_rise_tau 0 = off (no change; per-chain, so a driver-model chain that leaves it 0 is unaffected).
	// Timed sources only (needs the real dwell).
	if (as_point && m_vs.z_rise_tau > 0.0f
		&& prim->t0 >= 0.0 && prim->t1 > prim->t0)
	{
		const double dt_us = (prim->t1 - prim->t0) * 1e6;
		n *= float(1.0 - std::exp(-dt_us / double(m_vs.z_rise_tau)));
	}
	// Energy Jitter (near-saturation shimmer): a vector whose normalized energy n approaches
	// saturation wobbles by a band-limited PER-VECTOR random factor; dim vectors are untouched
	// (the weight hits 0 at the onset), unlike the retired whole-frame Vector Flicker. Applied to n
	// itself, BEFORE the transfer/overload chain, so below the two-regime threshold the brightness
	// wobbles, and above it the core stays saturated while width / white-pull / flare / Overload
	// Glow / halation shimmer - a bright beam that stays bright but trembles. The time axis is
	// emulated time quantized to energy_jitter_hz steps with smoothstep value-noise between steps
	// (bounded speed, freezes on pause); the per-vector seed hashes the quantized endpoints, so the
	// wobble is stable within a frame and independent between vectors, with no RNG state.
	const float jit = m_vs.energy_jitter;
	if (jit > 0.0f)
	{
		const float j_onset = m_vs.energy_jitter_onset;
		const float j_ramp  = std::max(0.05f, m_vs.energy_jitter_ramp);
		// Base floor: normal (below-onset) vectors still get a slight always-on wobble (analog-noise
		// shimmer of the whole image); the near-saturation ramp adds on top. energy_jitter_base 0 =
		// ramp only (prior behaviour - only near-saturation vectors wobble).
		const float j_w = std::max(std::clamp(m_vs.energy_jitter_base, 0.0f, 1.0f),
								   std::clamp((n - j_onset) / j_ramp, 0.0f, 1.0f));
		if (j_w > 0.0f)
		{
			const float j_hz = std::max(1.0f, m_vs.energy_jitter_hz);
			const double j_t = m_vec_time_ms * double(j_hz) * 0.001;
			const uint32_t j_step = uint32_t(int64_t(j_t));
			const float j_frac = float(j_t - double(j_step));
			auto jhash = [](uint32_t a) { a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16; return a; };
			const uint32_t j_seed = jhash(uint32_t(int32_t(x0 * 8.0f)) * 0x9e3779b9U)
								  ^ jhash(uint32_t(int32_t(y0 * 8.0f)) + 0x85ebca6bU)
								  ^ jhash(uint32_t(int32_t(x1 * 8.0f)) + 0xc2b2ae35U)
								  ^ jhash(uint32_t(int32_t(y1 * 8.0f)) + 0x27d4eb2fU);
			const float j_r0 = float(jhash(j_seed ^ jhash(j_step))      & 0xffffffu) / float(0x800000) - 1.0f;
			const float j_r1 = float(jhash(j_seed ^ jhash(j_step + 1u)) & 0xffffffu) / float(0x800000) - 1.0f;
			const float j_sm = j_frac * j_frac * (3.0f - 2.0f * j_frac);
			n *= std::max(0.0f, 1.0f + jit * j_w * (j_r0 + (j_r1 - j_r0) * j_sm));
		}
	}
	const float drive = std::clamp(n, 0.0f, 1.0f);
	// Stock fallback (chains without bright_threshold, e.g. default-vector): plain intensity-linear
	// response. The legacy intensity_clip_* / width_clip_* / intensity_curve transfer knobs are gone
	// (their last user, vector-vectrex-3d.json, was retired with the legacy chains); the two-regime
	// two-regime transfer below overrides both values on every phosphor chain.
	float display_a = drive;
	float wf = drive;
	// Two-regime transfer (bright_threshold > 0 enables it; 0 = stock, other chains unaffected):
	// brightness rises to max at the threshold T then SATURATES; energy above T is poured into the WIDTH
	// instead (a gentle width slope below T, a steep one above). width_knee = the width fraction reached
	// at T. b is the normalized beam energy (drive). This overrides display_a and wf computed above.
	const float bright_thresh = m_vs.bright_threshold;
	if (bright_thresh > 0.0f)
	{
		const float T = std::min(bright_thresh, 0.999f);
		display_a = std::clamp(drive / T, 0.0f, 1.0f);                      // brightness from clamped drive, saturates at T
		// Width uses the UNCLAMPED energy n (which can exceed 1), so once brightness saturates the beam
		// keeps getting thicker as energy rises (no width ceiling at n=1). bw_max = width at n=1.
		const float below = std::min(n, T) / T;                            // 0..1 over [0,T]
		// Width growth beyond beam_width_max once the beam is driven past the threshold (e.g. an object-lifted
		// bullet/explosion). The cap is the beam_width_overmax slider (multiple of the bw_min->bw_max span that the
		// "above" region can add): raise it so "lifted" objects get several times the normal width. Default 4.
		const float w_overmax = std::max(0.0f, m_vs.beam_width_overmax);
		float above = (n > T) ? std::min((n - T) / std::max(0.05f, 1.0f - T), w_overmax) : 0.0f;
		// width_over_curve shapes the overload-lift region ("above", the growth beyond beam_width_max)
		// the same way width_curve shapes the sub-threshold ramp below - width_curve deliberately excludes
		// this region (see the "leave the >1 region ... linear" note below), so without this the beam's
		// thickness always grew linearly with overload no matter how the OTHER overload-driven effects
		// (colour saturation, Overload Glow) were curved. pow(above/w_overmax, curve)*w_overmax keeps 0
		// and w_overmax fixed (only bends the path between), matching the wcurve convention.
		const float wocurve = m_vs.width_over_curve;
		if (wocurve != 1.0f && above > 0.0f && w_overmax > 0.0f) above = powf(above / w_overmax, wocurve) * w_overmax;
		const float knee  = std::clamp(m_vs.width_knee, 0.0f, 1.0f);
		wf = knee * below + (1.0f - knee) * above;                          // gentle below T, steep above (unbounded)
		// Optional shaping curves (powers) on top of the linear ramps: bright_curve bends the
		// energy->brightness response (>1 = darker mids / later rise to peak, <1 = brighter mids);
		// width_curve bends the energy->width response the same way. 1.0 = linear (unchanged).
		const float bcurve = m_vs.bright_curve;
		if (bcurve != 1.0f) display_a = powf(std::clamp(display_a, 0.0f, 1.0f), bcurve);
		const float wcurve = m_vs.width_curve;
		// Shape ONLY the min->max transition (wf in [0,1]); leave the >1 region (energy past the
		// threshold, where the beam grows beyond beam_width_max) linear. pow keeps 0->0 and 1->1, so
		// bw_min and bw_max stay fixed and the curve only bends the path between them - it must not
		// rescale the max itself.
		if (wcurve != 1.0f && wf > 0.0f && wf < 1.0f) wf = powf(wf, wcurve);
		// Optional sigmoid (S-curve) shaping ON TOP of the power curves, for a snappier CRT-like
		// brightness/width transition. bright_sigmoid/width_sigmoid > 0 = sigmoid (sharper contrast),
		// < 0 = softer (ease); 0 = off. *_sigmoid_center sets the inflection. Width keeps 0->0 / 1->1.
		const float bsig = m_vs.bright_sigmoid;
		if (bsig != 0.0f) display_a = vec_scurve(display_a, bsig, m_vs.bright_sigmoid_center);
		const float wsig = m_vs.width_sigmoid;
		if (wsig != 0.0f && wf > 0.0f && wf < 1.0f) wf = vec_scurve(wf, wsig, m_vs.width_sigmoid_center);
		// Normal-brightness cap, released when the beam is "lifted" (driven past the ref, n>1 = an
		// object-lifted bullet/explosion). Brightness is clamped to bright_normal_cap for normal beams and
		// the cap ramps back to 1.0 as n goes 1->2, so ordinary objects sit dimmer while lifted ones reach
		// full white. 1.0 = off (no cap).
		const float bcap = m_vs.bright_normal_cap;
		if (bcap < 1.0f)
		{
			const float uncap = std::clamp(n - 1.0f, 0.0f, 1.0f);   // 0 = normal (n<=1), 1 = lifted (n>=2)
			display_a = std::min(display_a, bcap + (1.0f - bcap) * uncap);
		}
	}
	const float bw_min = m_vs.beam_width_min;
	const float bw_max = m_vs.beam_width_max;
	// Width transfer. Chains with overload_width_add >= 0 use the three-region model: a slow normal
	// power curve up to brightness saturation, followed by a normalized logistic lift after the common
	// overload threshold. Chains without the new slider retain the former width transfer unchanged.
	const bool sigmoid_width = (m_vs.overload_width_add >= 0.0f);
	float overload_width_amount = 0.0f;
	float normal_beam_units;
	float beam_units;
	if (sigmoid_width)
	{
		const float width_T = (bright_thresh > 0.0f) ? std::min(bright_thresh, 0.999f) : 1.0f;
		float normal_width = std::clamp(n / width_T, 0.0f, 1.0f);
		const float normal_curve = std::max(0.01f, m_vs.width_curve);
		if (normal_curve != 1.0f) normal_width = powf(normal_width, normal_curve);
		beam_units = bw_min + normal_width * (bw_max - bw_min);
		normal_beam_units = beam_units;

		const float ow_thresh = m_vs.overload_threshold;
		const float ow_ramp = m_vs.overload_ramp;
		const float ow_span = (ow_ramp > 0.0f) ? ow_ramp : std::max(1e-3f, 1.0f - ow_thresh);
		const float ow_dot = std::max(1.0f, m_vs.overload_dot_gain);
		const float t = std::clamp((n - ow_thresh) / ow_span * (as_point ? ow_dot : 1.0f), 0.0f, 1.0f);
		if (t > 0.0f)
		{
			const float steep = std::max(0.01f, m_vs.overload_width_steepness);
			const float center = std::clamp(m_vs.overload_width_center, 0.01f, 0.99f);
			auto logistic = [steep, center](float x) { return 1.0f / (1.0f + expf(-steep * (x - center))); };
			const float l0 = logistic(0.0f), l1 = logistic(1.0f);
			overload_width_amount = std::clamp((logistic(t) - l0) / std::max(1e-6f, l1 - l0), 0.0f, 1.0f);
			beam_units += std::max(0.0f, m_vs.overload_width_add) * overload_width_amount;
		}
	}
	else
	{
		beam_units = bw_min + std::min(wf, 1.0f) * (bw_max - bw_min);
		normal_beam_units = beam_units;
		const float w_over_scale_slider = m_vs.beam_width_over_scale;
		const float w_over_scale = (w_over_scale_slider >= 0.0f) ? w_over_scale_slider : bw_max;
		if (wf > 1.0f) beam_units += (wf - 1.0f) * w_over_scale;
	}
	if (as_point)
	{
		beam_units *= m_vs.point_width_scale;
		normal_beam_units *= m_vs.point_width_scale;
	}
	float width = beam_units * (m_vec_res_w / 1920.0f);
	const float normal_width = std::max(0.5f, normal_beam_units * (m_vec_res_w / 1920.0f));
	const float ovld = 0.0f;
	if (width < 0.5f) width = 0.5f;

	// Intensity overrange (overdrive): display_a clamps at 1.0, so a single vector tops out at the beam
	// peak no matter how high beam_energy is - on its own it can only exceed peak by additive overlap.
	// To let a genuinely overdriven beam push past peak by itself (and so trip the present's overload
	// whitening), drive past the overload threshold adds a >1 factor carried per-vertex in the unused z
	// slot; fs_vector_line_analytic multiplies the deposit by (1+z), landing it in the float FBO above
	// 1.0. 0 = off (z stays 0 -> x1).
	// intensity_overdrive_curve shapes the ramp from the threshold (t=0) to full drive (t=1):
	// out = ov_gain * t^curve. curve > 1 keeps the white flare near zero until drive approaches 1.0 and
	// then rises sharply, so only the very brightest beams blow out white. curve = 1 is linear.
	const float ov_gain     = m_vs.intensity_overdrive;
	// Overload (white-hot) threshold: a beam with normalized energy n above this trips the flare.
	// Defaults to 1.0 (= peak) for chains without the slider.
	const float ov_thresh   = m_vs.overload_threshold;
	// Overload ramp: the n-span over which the overdrive/bloom ramp from the threshold toward full.
	// 0 = legacy (1 - threshold), which collapses to the 1e-3 guard (a step) once the threshold is at
	// or above 1.0 - set an explicit span to use thresholds > 1 (only object-lifted vectors overdrive,
	// ordinary peak-brightness lines/text stay out) while keeping a real gradation above it.
	const float ov_ramp     = m_vs.overload_ramp;
	const float ov_span     = (ov_ramp > 0.0f) ? ov_ramp : std::max(1e-3f, 1.0f - ov_thresh);
	// Dwell-dot preference: a parked beam (length-0 point) concentrates its energy in one spot, so the
	// same n reads far hotter than a swept line or a text stroke. Scaling the overdrive input for
	// points only lets bullets/stars sear while text (drawn as strokes) stays put - this decouples
	// "how hot are dots" from the threshold that keeps text out. 1 = no preference.
	const float ov_dot      = std::max(1.0f, m_vs.overload_dot_gain);
	float line_over = 0.0f;
	if (ov_gain > 0.0f)
	{
		// Drive the overdrive from the RAW (unclamped) beam energy, not the 0..1 display drive, so a
		// genuinely overdriven dwell beam (beam_energy can exceed 1.0 - Vectrex parks the beam and dumps
		// current into a point) flares far brighter than a peak-but-not-overdriven line. The curve shapes
		// the ramp from the knee to peak (1.0); above peak it grows linearly so true overdrive keeps
		// climbing instead of saturating at the same flare as a 1.0 line. Sources that never exceed 1.0
		// (AVG/DVG, color.a fallback) get ot<=1 and behave exactly as before.
		const float ot     = (n - ov_thresh) / ov_span * (as_point ? ov_dot : 1.0f);   // n above the overload threshold
		if (ot > 0.0f)
		{
			const float ocurve = m_vs.intensity_overdrive_curve;
			float shaped = (ot <= 1.0f) ? ((ocurve == 1.0f) ? ot : powf(ot, ocurve))
										: (1.0f + (ot - 1.0f));   // linear growth past peak
			// overload gain: sigmoid/logit shaping of the in-range overload about a movable centre
			// (0 = identity). The >1 (past-ref) part keeps its linear growth.
			const float og = m_vs.overload_gain;
			if (og != 0.0f)
				shaped = vec_scurve(std::min(shaped, 1.0f), og,
						m_vs.overload_gain_center) + std::max(0.0f, shaped - 1.0f);
			line_over = ov_gain * shaped;
			// Cap the overdrive multiplier: dwell points reach several x peak energy, and uncapped the
			// (1+z) deposit would land tens of x peak in the float FBO - the phosphor pool then holds that
			// peak and the emit stays saturated for most of the decay (long burnt trails), and the present
			// roll-off crushes everything to its ceiling anyway. overload_max = the largest deposit
			// multiple of peak a single vector may reach (0 = uncapped, prior behaviour).
			const float ov_max = m_vs.overload_max;
			if (ov_max > 0.0f) line_over = std::min(line_over, std::max(0.0f, ov_max - 1.0f));
		}
	}
	// Route the overdrive into the CORE deposit (body / caps / dwell-dot z): the flare quad lands in
	// the glow FBO, which chains like vector-vectrex composite at tiny glow weights (~0.02),
	// so there the flare alone cannot brighten the spot. With overdrive_core > 0 the same (1+z)
	// overrange multiplies the core deposit itself into the float FBO, where it feeds the phosphor
	// pool and the bloom cascade like any other light. 0 = off (flare-only, prior behaviour).
	float core_over = line_over * std::clamp(m_vs.overdrive_core, 0.0f, 1.0f);
	// Colour-linear brightness (3D imager / colour sources): wheel-segment colour composition encodes
	// the hue in the per-pass INTENSITY RATIOS, and the two-regime transfer erases them for bright
	// objects - display_a saturates at T, so every channel of a bright ship deposits at 1.0 and the
	// ship turns WHITE. At 1.0 the deposit is LINEAR in the beam energy instead (min(n,1) in the
	// 8-bit alpha, the remainder in the (1+z) overrange - the float FBO / pool / present roll-off
	// carry it), preserving channel ratios at any brightness. 0 = mono two-regime (unchanged).
	const float lin_col = std::clamp(m_vs.linear_color, 0.0f, 1.0f);
	if (lin_col > 0.0f)
	{
		display_a = display_a + lin_col * (std::min(n, 1.0f) - display_a);
		core_over = core_over + lin_col * (std::max(n - 1.0f, 0.0f) - core_over);
	}

	// The legacy length-fade (vector_length_scale/ratio), dot_boost and dwell_* brightness knobs
	// are gone - the unified energy model (speed / dwell derived from the per-segment timestamps)
	// covers all three.
	float length_factor = 1.0f;

	// HV supply droop: a bright/busy frame sags the EHT supply, dimming the
	// whole picture (here) and defocusing the spot (sigma, below). m_hv_load_norm is the smoothed 0..1
	// frame load; hv_droop scales the effect (0 = off). The dim is capped at 0.4 of full brightness.
	const float hv_droop = m_vs.hv_droop;
	if (hv_droop > 0.0f && m_hv_load_norm > 0.0f)
		length_factor *= (1.0f - hv_droop * 0.4f * m_hv_load_norm);

	// Overdrive white-out lives in the glow buffer (post shadow-mask), NOT here: a beam driven past
	// the overload threshold deposits an UNMASKED white flare (below) so the white bloom is not patterned by
	// the shadow mask. The masked core stays the ordinary coloured line at its normal intensity.

	// Per-vector phosphor-tint white-pull (moved here from the old "Phosphor Tint" chain pass, which
	// drove it from the COMPOSITED pixel brightness sampled after all of this frame's additive line
	// draws had already summed - so several ordinary, non-overloaded vectors overlapping the same
	// pixel (dense debris/explosion combos) could read as "past the knee" and blow out white even
	// though no single vector was ever in overload. Gating on line_over>0 (a per-vector, pre-blend
	// signal, exactly like the flare/width overload paths) fixes that while keeping the SAME
	// knee/ceiling/curve/amount/colour calibration - just decided per-vector instead of per-pixel.
	float core_sat_r = prim->color.r, core_sat_g = prim->color.g, core_sat_b = prim->color.b;
	if (line_over > 0.0f)
	{
		const float sat_amt = m_vs.phosphor_overdrive;
		if (sat_amt > 0.0f)
		{
			const float sat_knee = m_vs.overdrive_knee;
			// ceiling is pre-guarded to knee + eps by the slider-cache refresh
			const float sat_ceil = m_vs.overdrive_ceil;
			float sw = std::clamp((n - sat_knee) / (sat_ceil - sat_knee), 0.0f, 1.0f);
			const float sat_curve = m_vs.overdrive_sat_curve;
			if (sat_curve != 1.0f) sw = powf(sw, sat_curve);
			sw *= sat_amt;
			if (sw > 0.0f)
			{
				const float ov_r = m_vs.overdrive_color[0];
				const float ov_g = m_vs.overdrive_color[1];
				const float ov_b = m_vs.overdrive_color[2];
				const float mag = std::max({ core_sat_r, core_sat_g, core_sat_b, 1e-4f });
				core_sat_r += (ov_r * mag - core_sat_r) * sw;
				core_sat_g += (ov_g * mag - core_sat_g) * sw;
				core_sat_b += (ov_b * mag - core_sat_b) * sw;
			}
		}
	}

	// Point Brightness Scale changes only the point core deposit.  Keep the alpha within the
	// packed colour range and carry any value above 1.0 in z, matching the existing HDR core path.
	float core_alpha = std::clamp(display_a, 0.0f, 1.0f);
	float point_core_over = core_over;
	if (as_point)
	{
		const float point_scale = std::max(0.0f, m_vs.point_brightness_scale);
		const float point_peak = core_alpha * (1.0f + std::max(0.0f, core_over)) * point_scale;
		core_alpha = std::min(point_peak, 1.0f);
		point_core_over = (core_alpha > 1.0e-6f) ? std::max(0.0f, point_peak / core_alpha - 1.0f) : 0.0f;
	}

	// clamp: length_factor can exceed 1.0 with the dwell-time boost, and u32Color does not clamp
	const uint32_t rgba = u32Color(
		std::min<uint32_t>(uint32_t(core_sat_r * length_factor * 255.0f + 0.5f), 255),
		std::min<uint32_t>(uint32_t(core_sat_g * length_factor * 255.0f + 0.5f), 255),
		std::min<uint32_t>(uint32_t(core_sat_b * length_factor * 255.0f + 0.5f), 255),
		uint32_t(core_alpha * 255.0f + 0.5f));

	// Overdrive white flare encoding (deposited into the glow buffer = post shadow-mask, so it is not
	// patterned by the mask). White, peak proportional to the overdrive; peak > 1 is carried in z (the
	// shader multiplies the deposit by 1+z) exactly like the body overrange. flare_on gates the slot.
	const bool  flare_on   = (line_over > 0.0f);
	const float flare_peak = std::clamp(display_a, 0.0f, 1.0f) * line_over * length_factor;
	const float flare_z    = std::max(0.0f, flare_peak - 1.0f);
	// Flare colour = the beam's own hue at full strength, NOT fixed white: the 3D imager (and any
	// colour source) puts a colour filter in front of the whole tube, so even the white-hot overload
	// flare arrives tinted - a fixed white flare injected achromatic light into pure-colour scenes
	// and washed bright objects (the imager ship) toward white. A monochrome (white) beam yields
	// exactly the old white flare, so mono chains are unchanged.
	const float flare_pk = std::max(std::max(prim->color.r, prim->color.g), std::max(prim->color.b, 1e-4f));
	const uint32_t flare_rgba = u32Color(
		uint32_t(std::min(prim->color.r / flare_pk, 1.0f) * 255.0f + 0.5f),
		uint32_t(std::min(prim->color.g / flare_pk, 1.0f) * 255.0f + 0.5f),
		uint32_t(std::min(prim->color.b / flare_pk, 1.0f) * 255.0f + 0.5f),
		uint32_t(std::min(1.0f, flare_peak) * 255.0f + 0.5f));

	// Overload Glow (bloom that fires ONLY on overload, distinct from the generic per-line
	// "analytic_glow" which scales with plain brightness regardless of overdrive state, and from
	// "Overload Bloom" which only widens the beam's OWN sigma - a bigger/softer spot, not a halo
	// reaching pixels well away from the geometry). This reuses the SAME wide-cascade glow FBO
	// pipeline as analytic_glow (glow_narrow/glow_wide), just with its own heat-gated magnitude and
	// an independently wide sigma, so a hot dwell dot or overdriven line gets a real screen-space
	// bloom that ordinary (non-overloaded) content never triggers. 0 = off.
	const float oglow_gain = m_vs.overload_glow_gain;
	const bool  oglow_on   = flare_on && oglow_gain > 0.0f;
	const float oglow_mag  = flare_peak * oglow_gain;
	const float oglow_z    = std::max(0.0f, oglow_mag - 1.0f);
	const uint32_t oglow_rgba = u32Color(
		uint32_t(std::min(prim->color.r / flare_pk, 1.0f) * 255.0f + 0.5f),
		uint32_t(std::min(prim->color.g / flare_pk, 1.0f) * 255.0f + 0.5f),
		uint32_t(std::min(prim->color.b / flare_pk, 1.0f) * 255.0f + 0.5f),
		uint32_t(std::min(1.0f, oglow_mag) * 255.0f + 0.5f));

	// Core, caps, flare and glow share one width. Overload Bloom uses the same normalized sigmoid
	// amount as the geometric lift, so both remain quiet initially and rise together later.
	float sigma = (width / 3.2f) * (1.0f + ovld);
	const float overload_bloom = m_vs.overload_bloom;
	if (overload_bloom > 0.0f)
	{
		const float over_e = sigmoid_width
			? overload_width_amount
			: std::clamp((n - ov_thresh) / ov_span * (as_point ? ov_dot : 1.0f), 0.0f, 1.0f);
		if (over_e > 0.0f)
			sigma += overload_bloom * (m_vec_res_w / 1920.0f) * over_e * 4.0f;
	}
	// Edge defocus (per jmargolin.com/vgens): at large deflection angles the spot defocuses
	// astigmatically, so sigma grows toward the screen edges. The segment midpoint's radius is
	// normalised to the half-diagonal (0 at centre, 1 at a corner) and raised to edge_defocus_curve
	// (2 = quadratic, matching deflection-angle growth). edge_defocus 0 = off.
	const float edge_def = m_vs.edge_defocus;
	if (edge_def > 0.0f)
	{
		const float sw = float(s_width[window().index()]);
		const float sh = float(s_height[window().index()]);
		const float halfdiag = 0.5f * sqrtf(sw * sw + sh * sh);
		const float mx = (x0 + x1) * 0.5f - sw * 0.5f;
		const float my = (y0 + y1) * 0.5f - sh * 0.5f;
		float r = (halfdiag > 0.0f) ? std::min(sqrtf(mx * mx + my * my) / halfdiag, 1.0f) : 0.0f;
		const float ecurve = m_vs.edge_defocus_curve;
		sigma += edge_def * (m_vec_res_w / 1920.0f) * powf(r, ecurve);
	}
	// HV droop defocus: the same supply sag that dims the picture widens the spot (capped ~2.5 px at
	// 1920-ref, scaled by the load). Pairs with the dim applied to length_factor above.
	if (hv_droop > 0.0f && m_hv_load_norm > 0.0f)
		sigma += hv_droop * 2.5f * (m_vec_res_w / 1920.0f) * m_hv_load_norm;
	// Rasterization floor so a sub-pixel gaussian does not fall between fragment centres and
	// vanish. Points now use the same box-integrated AA as lines (see fs_vector_line_analytic point
	// mode), so they can share the line's thin floor - this keeps a point and a connected line of equal
	// intensity the same size/brightness (previously the point's larger 0.85 floor made it read brighter).
	const float sig_floor = 0.33f;
	if (sigma < sig_floor) sigma = sig_floor;
	// Flat-core width/sigma decoupling (core_flat > 0): sigma = width/3.2 couples the blur to the
	// width, so a width-lifted (overdriven) beam became one wide soft blob. Carve the cross-section
	// into a SOLID band/disc of half-width wcore with only a thin gaussian skirt outside it (the
	// shader shifts the gaussian by wcore): the beam then reads as a bright band with crisp edges.
	// sigma keeps (1 - core_flat) of its value for the skirt. 0 = off (plain gaussian, exact prior
	// behaviour - other chains unaffected).
	const float core_flat = std::clamp(m_vs.core_flat, 0.0f, 0.98f);
	// Points and lines share the same flat-core fraction (the separate dot_flat override was retired;
	// it had defaulted to the same value as core_flat, so this is behaviour-preserving).
	const float flat_f = core_flat;
	float wcore = 0.0f;
	if (flat_f > 0.0f)
	{
		wcore = flat_f * 0.5f * width;
		// Edge skirt sigma from the remaining (1 - F) share of the gaussian.
		sigma = std::max(sig_floor, sigma * (1.0f - flat_f));
	}
	// Overload Glow (bloom) sigma: add the wide-halo width on top of the shared beam sigma (same
	// pattern as analytic_glow's glow_sig = sigma + glow_w). The width term is scaled by oglow_ramp
	// (0..1, saturating with how deep into overload this vector is - same saturation point as the
	// glow's own alpha, oglow_mag) rather than added flat: without this, a vector that barely crosses
	// overload_threshold (line_over ~ 0.001, near-invisible glow) still paid the FULL wide-quad
	// rasterization cost (width~40px -> ~300px quad side, ~90K px^2) as a vector deep in overload. In
	// a mass-overload scene (explosion) most vectors are only marginally over threshold, so this was
	// spending near-max fill-rate on near-zero-alpha quads across potentially hundreds of vectors -
	// the actual cause of the Death Star explosion frame-rate drop. Ramping footprint with intensity
	// keeps the dramatic wide halo for genuinely hot vectors while making barely-overloaded ones cheap.
	const float oglow_ramp = std::min(1.0f, oglow_mag);
	const float oglow_sig = sigma + std::max(0.0f, m_vs.overload_glow_width * (m_vec_res_w / 1920.0f)) * oglow_ramp;
	const float pad = wcore + 3.5f * sigma + 0.5f;

	if (seg_len > 0.0001f) { const float inv = 1.0f / seg_len; dx *= inv; dy *= inv; }
	else { dx = 1.0f; dy = 0.0f; }
	const float nx = dy, ny = -dx;

	auto setv = [&](int i, float x, float y, float a, float b, float d, float sg) {
		vertex[i].m_x = x; vertex[i].m_y = y; vertex[i].m_z = core_over;
		vertex[i].m_rgba = rgba;
		vertex[i].m_u = wcore; vertex[i].m_v = 0.0f;
		vertex[i].m_a = a; vertex[i].m_b = b; vertex[i].m_d = d; vertex[i].m_sigma = sg;
	};

	// 2D gaussian dot quad (point mode: sigma sign flags it in the shader). tgt selects the core or the
	// glow buffer (halation ring / inner fill go to the glow buffer so they are not shadow-masked).
	// zval = intensity overrange carried in z (core dot / caps pass line_over; glow / ring pass 0).
	auto set_dot = [&](AnalyticLineVertex* tgt, int base, float cx, float cy, float sg_abs, uint32_t drgba, float zval, float wc) {
		const float p = wc + 3.5f * sg_abs + 0.5f;
		const float sg = -sg_abs;
		auto dv = [&](int i, float x, float y, float a, float d) {
			tgt[i].m_x = x; tgt[i].m_y = y; tgt[i].m_z = zval;
			tgt[i].m_rgba = drgba;
			tgt[i].m_u = wc; tgt[i].m_v = 0.0f;
			tgt[i].m_a = a; tgt[i].m_b = 0.0f; tgt[i].m_d = d; tgt[i].m_sigma = sg;
		};
		dv(base + 0, cx - p, cy - p, -p, -p);
		dv(base + 1, cx + p, cy - p,  p, -p);
		dv(base + 2, cx + p, cy + p,  p,  p);
		dv(base + 3, cx - p, cy - p, -p, -p);
		dv(base + 4, cx + p, cy + p,  p,  p);
		dv(base + 5, cx - p, cy + p, -p,  p);
	};
	auto set_degenerate = [&](AnalyticLineVertex* tgt, int base) {
		for (int i = 0; i < 6; i++)
		{
			tgt[base + i].m_x = x0; tgt[base + i].m_y = y0; tgt[base + i].m_z = 0.0f;
			tgt[base + i].m_rgba = 0;
			tgt[base + i].m_u = 0.0f; tgt[base + i].m_v = 0.0f;
			tgt[base + i].m_a = 0.0f; tgt[base + i].m_b = 0.0f; tgt[base + i].m_d = 0.0f; tgt[base + i].m_sigma = -1.0f;
		}
	};

	// Halation ring: one smooth circle centred on the dot (b = radius flags ring mode in the
	// shader; sigma = -edge width). A single quad per bullet, so it is continuous, not a ring of
	// gather dots.
	auto set_ring = [&](AnalyticLineVertex* tgt, int base, float cx, float cy, float radius, float width, uint32_t rrgba, float zval) {
		const float p = radius + 3.0f * width + 1.0f;  // quad half-extent covers the soft rim
		const float sg = -width;
		auto rv = [&](int i, float x, float y, float a, float d) {
			tgt[i].m_x = x; tgt[i].m_y = y; tgt[i].m_z = zval;
			tgt[i].m_rgba = rrgba;
			tgt[i].m_u = 0.0f; tgt[i].m_v = 0.0f;
			tgt[i].m_a = a; tgt[i].m_b = radius; tgt[i].m_d = d; tgt[i].m_sigma = sg;
		};
		rv(base + 0, cx - p, cy - p, -p, -p);
		rv(base + 1, cx + p, cy - p,  p, -p);
		rv(base + 2, cx + p, cy + p,  p,  p);
		rv(base + 3, cx - p, cy - p, -p, -p);
		rv(base + 4, cx + p, cy + p,  p,  p);
		rv(base + 5, cx - p, cy + p, -p,  p);
	};

	// Analytic glow (additional-ideas A-1b): a wide, low-amplitude gaussian copy of the same
	// primitive, drawn additively into the FBO. Being analytic it tracks the beam exactly (no pyramid,
	// no temporal lag) and its broad tail accumulates across lines into the scene glow. The glow quad
	// goes in the slots right after the body + caps. m_glow_on gates it (analytic_glow 0 = off).
	const float glow_str  = m_vs.analytic_glow;
	const float glow_w    = m_vs.analytic_glow_width * (m_vec_res_w / 1920.0f);
	const float glow_sig  = sigma + std::max(0.0f, glow_w);
	// Glow onset: only sources brighter than glow_threshold glow, ramped by glow_curve - so faint
	// stars stay dark while bright bullets/explosions bloom. glow_threshold 0 + glow_curve 1 reproduce
	// the old linear behaviour (magnitude = colour x length_factor x analytic_glow) exactly. The hue is
	// preserved (colour normalised by its peak) and the magnitude carries the shaped intensity.
	const float glow_thr  = m_vs.glow_threshold;
	const float glow_crv  = m_vs.glow_curve;
	const float g_bI    = std::max(std::max(prim->color.r, prim->color.g), prim->color.b) * length_factor;
	const float g_onset = std::max(0.0f, g_bI - glow_thr);
	const float g_mag   = glow_str * ((glow_crv == 1.0f) ? g_onset : powf(g_onset, glow_crv));
	const float g_peak  = std::max(std::max(std::max(prim->color.r, prim->color.g), prim->color.b), 1e-4f);
	const float g_scale = g_mag / g_peak;
	const uint32_t glow_rgba = u32Color(
		std::min<uint32_t>(uint32_t(prim->color.r * g_scale * 255.0f + 0.5f), 255),
		std::min<uint32_t>(uint32_t(prim->color.g * g_scale * 255.0f + 0.5f), 255),
		std::min<uint32_t>(uint32_t(prim->color.b * g_scale * 255.0f + 0.5f), 255),
		uint32_t(std::clamp(display_a, 0.0f, 1.0f) * 255.0f + 0.5f));

	if (as_point)
	{
		const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
		// CORE: the sharp dwelling-beam spot (emission - masked normally by the shadow mask). Blank the
		// rest of the core buffer; the halation ring / inner fill now live in the glow buffer instead.
		// The dot carries the same colour as a line of equal intensity: the shader's box-integrated AA
		// already saturates the spot peak to 1 once it covers a fragment, so a resolved dot's brightness
		// is independent of its size. Brightness therefore comes purely from the intensity transfer
		// (display_a in rgba), optionally scaled by point_brightness_scale; point_width_scale sets only
		// the spot size. No energy normalisation -
		// scaling the peak by 1/sigma^2 made a larger dot dimmer, so point_width_scale brightened then
		// darkened the dot instead of simply growing it.
		// Short-dwell (junction) dot no-persistence: a length-0 dot parked only a few us is the beam
		// pausing between split line segments (BIOS) - real phosphor gets almost no extra energy
		// there, but the pool's peak-hold let these dots outlast the line body in a moving object's
		// trail (a dotted line of ghosts). Route them through the separate NO-PERSIST FBO (post-pool,
		// like the caps): visible while drawn, no afterimage, and never fed into the glow cascade. Real
		// object dots (bullets/stars, dwell tens of us and up) keep the normal pool path. 0 = off.
		bool dot_np = false;
		if (np_vertex != nullptr)
		{
			const float np_us = m_vs.dot_no_persist_dwell;
			if (np_us > 0.0f && prim->t0 >= 0.0 && prim->t1 > prim->t0
				&& (prim->t1 - prim->t0) * 1e6 < double(np_us))
				dot_np = true;
		}
		if (dot_np)
		{
			// dedicated FBO: no combine-gain compensation, same z as the scaled core spot
			set_dot(np_vertex, 0, cx, cy, sigma, rgba, point_core_over, wcore);
			set_degenerate(vertex, 0);
		}
		else
		{
			set_dot(vertex, 0, cx, cy, sigma, rgba, point_core_over, wcore);
			if (np_vertex) set_degenerate(np_vertex, 0);
		}
		for (int g = 1; g * 6 < int(m_vec_vpl); g++)
			set_degenerate(vertex, g * 6);
		// point path uses no cap slots in the no-persist buffer
		if (np_vertex) { set_degenerate(np_vertex, 6); set_degenerate(np_vertex, 12); }

		// GLOW buffer (composited AFTER the shadow mask, so this scattered light is not mask-patterned):
		// the wide analytic glow dot + the halation ring + the inner fill.
		AnalyticLineVertex *const optics_vertex = m_optical_separate ? optical_vertex : glow_vertex;
		if (glow_vertex || optics_vertex || ray_vertex)
		{
			// analytic glow dot (glow_rgba is 0 when analytic_glow is off -> invisible)
			if (glow_vertex && m_glow_off_glow >= 0) set_dot(glow_vertex, m_glow_off_glow, cx, cy, glow_sig, glow_rgba, 0.0f, 0.0f);

			// Halation around bright dwell dots (bullets). The rendered brightness includes the dwell
			// boost, so only bright dots reach the threshold; the rim (gain) and the inner fill have
			// independent brightness so the fill stays visible when the rim is dialed right down.
			const float ring_gain = m_vs.ring_gain;
			const float ring_fill = m_vs.ring_fill;
			const float halation_gain = std::max(0.0f, m_vs.halation_gain);
			// Halation-from-overdrive (ring_over_gain > 0): the ring stops using the legacy brightness
			// threshold and instead follows the overdrive heat - strength scales with line_over (the
			// capped overrange), so only genuinely hot dwell dots grow the "angel ring" and it swells
			// with the heat. The explicit optical FBO bypasses ordinary glow gain and tail shaping,
			// so the rim lands at its tuned strength independently. 0 = legacy threshold gate.
			const float ring_link = m_vs.ring_over_gain;
			float ring_str = 1.0f;
			const float eff_bright = std::max(std::max(prim->color.r, prim->color.g), prim->color.b) * length_factor;
			bool ring_on;
			if (ring_link > 0.0f)
			{
				ring_str = ring_link * line_over;

				ring_on = halation_gain > 0.0f && (ring_gain > 0.0f || ring_fill > 0.0f) && ring_str > 0.0f;
			}
			else
			{
				ring_on = halation_gain > 0.0f && (ring_gain > 0.0f || ring_fill > 0.0f)
					&& eff_bright >= m_vs.ring_threshold;
			}
			// Small-text leak: tiny text strokes move the beam less than the point threshold, so they
			// classify as points, and the driver's text clamp is LINE-only (a parked dot's leftover T1
			// scale is not a reliable text marker) - so a lifted text sub-dot can reach n>1 and ring.
			// A real halation dot is a PARKED beam: gate on the per-vector dwell time (text sub-dots
			// last a few us while bullet/star dwell dots park for tens to hundreds). Untimed sources
			// (t0/t1 < 0) pass the gate unchanged. 0 = off.
			const float ring_min_dwell = m_vs.ring_min_dwell;   // us
			if (ring_on && ring_min_dwell > 0.0f && prim->t0 >= 0.0 && prim->t1 > prim->t0
				&& (prim->t1 - prim->t0) * 1e6 < double(ring_min_dwell))
				ring_on = false;
			const float res = m_vec_res_w / 1920.0f;
			const float radius = std::max(2.0f, m_vs.ring_radius * res);
			const float da = std::clamp(display_a, 0.0f, 1.0f);
			auto ring_color = [&](float strength) -> uint32_t {
				return u32Color(
					std::min<uint32_t>(uint32_t(prim->color.r * length_factor * strength * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(prim->color.g * length_factor * strength * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(prim->color.b * length_factor * strength * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(da * 255.0f + 0.5f), 255));
			};
			const float optical_gain_comp = (!m_optical_separate && m_vs.glow_narrow > 1e-4f)
				? (1.0f / m_vs.glow_narrow) : 1.0f;
			// Rim at slot 6, inner fill at slot 12. Keep colour at useful RGBA8 precision and carry the
			// complete sub-1.0 intensity in the float z channel: baking tiny strengths into the vertex colour
			// quantized them to zero/one and made the controls switch abruptly. Linear gain response restores
			// a genuinely faint low end without quantization.
			if (optics_vertex && m_glow_off_ring >= 0)
			{
				if (ring_on && ring_gain > 0.0f)
				{
					const float width = std::max(0.75f, m_vs.ring_width * res);
					set_ring(optics_vertex, m_glow_off_ring, cx, cy, radius, width, ring_color(1.0f), ring_gain * 0.05f * ring_str * halation_gain * optical_gain_comp - 1.0f);
				}
				else
					set_degenerate(optics_vertex, m_glow_off_ring);
			}
			if (optics_vertex && m_glow_off_fill >= 0)
			{
				if (ring_on && ring_fill > 0.0f)
					set_dot(optics_vertex, m_glow_off_fill, cx, cy, std::max(1.0f, radius * 0.5f), ring_color(1.0f), ring_fill * 0.04f * ring_str * halation_gain * optical_gain_comp - 1.0f, 0.0f);
				else
					set_degenerate(optics_vertex, m_glow_off_fill);
			}
			// Overdrive white flare (slot 18): an overdriven dwell dot blooms white-hot here in the glow
			// buffer (post-mask), at the dot's own size, so it is not patterned by the shadow mask.
			if (glow_vertex && m_glow_off_flare >= 0)
			{
				if (flare_on)
					set_dot(glow_vertex, m_glow_off_flare, cx, cy, sigma, flare_rgba, flare_z, wcore);
				else
					set_degenerate(glow_vertex, m_glow_off_flare);
			}
			// Overload Glow (bloom): a wide soft halo, gated on the SAME heat as the flare, feeding the
			// existing glow_narrow/glow_wide cascade for a real screen-space spread ordinary brightness
			// never triggers (see the oglow_* comment above for why this differs from analytic_glow /
			// Overload Bloom).
			if (glow_vertex && m_glow_off_oglow >= 0)
			{
				if (oglow_on)
					set_dot(glow_vertex, m_glow_off_oglow, cx, cy, oglow_sig, oglow_rgba, oglow_z, 0.0f);
				else
					set_degenerate(glow_vertex, m_glow_off_oglow);
			}
			// Starburst rays: the straight radial streaks the EYE sees around a genuinely dazzling
			// point light (lash / lens diffraction) - the display cannot reach that luminance, so the
			// percept is drawn explicitly. Only hot dwell dots (overdrive heat > 0) get rays; ALL dots
			// share ONE fixed orientation (the pattern belongs to the viewer's eye, not the tube), and
			// both the length and the brightness grow with the heat, so rays fade in as a dot heats up.
			// Uses the same min-dwell text gate as the halation ring.
			if (ray_vertex != nullptr)
			{
				const float ray_gain = m_vs.ray_gain;
				const float rov_max  = m_vs.overload_max;
				const float heat = std::clamp(line_over / ((rov_max > 1.0f) ? (rov_max - 1.0f) : 2.0f), 0.0f, 1.0f);
				bool rays_on = (ray_gain > 0.0f && heat > 0.0f);
				if (rays_on && ring_min_dwell > 0.0f && prim->t0 >= 0.0 && prim->t1 > prim->t0
					&& (prim->t1 - prim->t0) * 1e6 < double(ring_min_dwell))
					rays_on = false;
				const float rsig = std::max(0.4f, m_vs.ray_width * res);
				const float rlen = m_vs.ray_length * res * heat;
				const float rang = m_vs.ray_angle * 0.017453293f;
				const float ray_gain_comp = (!m_optical_separate && m_vs.glow_narrow > 1e-4f)
					? (1.0f / m_vs.glow_narrow) : 1.0f;
				// Uneven ray lengths (a real eye starburst is irregular): a FIXED per-ray-index pattern,
				// identical for every dot (the pattern belongs to the viewer's eye, not the tube) and
				// stable across frames. ray_var blends from all-equal (0) to the full pattern (1).
				static const float ray_var_tbl[12] = { 1.00f, 0.52f, 0.83f, 0.40f, 0.95f, 0.58f, 0.72f, 0.45f, 0.88f, 0.62f, 0.78f, 0.50f };
				const float ray_var = std::clamp(m_vs.ray_var, 0.0f, 1.0f);
				// Hue-only ray colour (bypasses ring_color's length_factor, which can push hot dots'
				// bytes into the 255 clamp and break the faint-streak encoding below). Alpha = 1.
				const float rpk = std::max(std::max(prim->color.r, prim->color.g), std::max(prim->color.b, 1e-4f));
				auto ray_color = [&](float strength) -> uint32_t {
					const float sN = strength / rpk;
					return u32Color(
						std::min<uint32_t>(uint32_t(prim->color.r * sN * 255.0f + 0.5f), 255),
						std::min<uint32_t>(uint32_t(prim->color.g * sN * 255.0f + 0.5f), 255),
						std::min<uint32_t>(uint32_t(prim->color.b * sN * 255.0f + 0.5f), 255),
						255);
				};
				// Taper: a real glare streak widens and fades with distance until it dissolves into the
				// background - a constant-brightness quad reads as a drawn LINE instead. Three abutting
				// sub-segments per ray, sigma growing and gain dropping outward; collinear analytic
				// segments join smoothly (each erf end contributes exactly 0.5 at the shared point).
				static const float seg_f0[GLOW_RAY_SEGS] = { 0.00f, 0.30f, 0.62f };
				static const float seg_f1[GLOW_RAY_SEGS] = { 0.30f, 0.62f, 1.00f };
				static const float seg_sg[GLOW_RAY_SEGS] = { 1.0f,  1.9f,  3.2f };
				static const float seg_g [GLOW_RAY_SEGS] = { 1.0f,  0.42f, 0.16f };
				// Time-varying ray-length wobble (ray_length_rand > 0): each ray stretches/shrinks over
				// time so the starburst shimmers rather than sitting rigid. Per (dot, ray-index) seed,
				// smoothstep value-noise on the shared jitter cadence (freezes on pause). 0 = static.
				const float rl_rand = std::clamp(m_vs.ray_length_rand, 0.0f, 1.0f);
				const float rc_rand = std::clamp(m_vs.ray_count_rand, 0.0f, 1.0f);
				auto rh = [](uint32_t a) { a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16; return a; };
				const float rl_hz = std::max(1.0f, m_vs.energy_jitter_hz);
				const double rl_t = m_vec_time_ms * double(rl_hz) * 0.001;
				const uint32_t rl_step = uint32_t(int64_t(rl_t));
				const float rl_fr = float(rl_t - double(rl_step));
				const float rl_sm = rl_fr * rl_fr * (3.0f - 2.0f * rl_fr);
				const uint32_t rl_dot = rh(uint32_t(int32_t(cx))) ^ rh(uint32_t(int32_t(cy)) + 0x9e3779b9U);
				for (int ri = 0; ri < m_glow_rays_n; ri++)
				{
					const float th = rang + float(ri) * (6.2831853f / float(m_glow_rays_n));
					const float ux = cosf(th), uy = sinf(th);
					const float rnx = uy, rny = -ux;
					float rlen_i = rlen * (1.0f - ray_var * (1.0f - ray_var_tbl[ri % 12]));
					if (rl_rand > 0.0f)
					{
						const uint32_t rs = rl_dot ^ rh(uint32_t(ri) * 0x9e3779b9U + 1u);
						const float n0 = float(rh(rs ^ rh(rl_step))      & 0xffffffu) / float(0x800000) - 1.0f;
						const float n1 = float(rh(rs ^ rh(rl_step + 1u)) & 0xffffffu) / float(0x800000) - 1.0f;
						rlen_i *= std::max(0.05f, 1.0f + rl_rand * (n0 + (n1 - n0) * rl_sm));
					}
					// Random ray COUNT: independently suppress rays over time so the visible ray count
					// varies (the buffer is still sized for the max; a suppressed ray is drawn degenerate
					// via the rlen_i < 1 test below). ray_count_rand = fraction of rays randomly dropped;
					// a soft band around the threshold fades a ray out (shrinks its length to 0) rather
					// than popping. Time-coherent per (dot, ray), so rays blink in/out at a bounded rate.
					if (rc_rand > 0.0f)
					{
						const uint32_t cs = rl_dot ^ rh(uint32_t(ri) * 0x85ebca6bU + 0x9999u);
						const float c0 = float(rh(cs ^ rh(rl_step))      & 0xffffffu) / float(0xffffffu);
						const float c1 = float(rh(cs ^ rh(rl_step + 1u)) & 0xffffffu) / float(0xffffffu);
						const float cn = c0 + (c1 - c0) * rl_sm;   // 0..1 time-coherent
						rlen_i *= std::clamp((cn - rc_rand) / 0.15f, 0.0f, 1.0f);
					}
					for (int sj = 0; sj < GLOW_RAY_SEGS; sj++)
					{
						const int rbase = (ri * GLOW_RAY_SEGS + sj) * 6;
						if (!rays_on || rlen_i < 1.0f)
						{
							set_degenerate(ray_vertex, rbase);
							continue;
						}
						const float l0 = rlen_i * seg_f0[sj];
						const float slen = rlen_i * (seg_f1[sj] - seg_f0[sj]);
						const float ssig = rsig * seg_sg[sj];
						// Sub-1/255 dimming: rgb bytes floor at 1/255, which on a black background is
						// still a clearly visible streak once the glow-weight compensation multiplies it
						// back up. Encode the desired deposit as byte x (1+z) instead: faint targets use
						// z = 0 with a sub-0.5 byte (down to 1/255 x glow weight), bright ones a fixed
						// 0.5 byte with z carrying the rest.
						const float want = ray_gain * heat * seg_g[sj] * ray_gain_comp;
						const float sstr = std::min(want, 0.5f);
						const float szz  = (want > 0.5f) ? (want * 2.0f - 1.0f) : 0.0f;
						const uint32_t srgba = ray_color(sstr);
						const float rpad = 3.5f * ssig + 0.5f;
						const float bx = cx + ux * l0, by = cy + uy * l0;   // sub-segment start
						const float sx0 = bx - ux * rpad, sy0 = by - uy * rpad;
						const float sx1 = bx + ux * (slen + rpad), sy1 = by + uy * (slen + rpad);
						const float a0 = -rpad, a1 = slen + rpad;
						auto rvv = [&](int i, float x, float y, float a, float d) {
							ray_vertex[i].m_x = x; ray_vertex[i].m_y = y; ray_vertex[i].m_z = szz;
							ray_vertex[i].m_rgba = srgba;
							ray_vertex[i].m_u = 0.0f; ray_vertex[i].m_v = 0.0f;
							ray_vertex[i].m_a = a; ray_vertex[i].m_b = a - slen; ray_vertex[i].m_d = d; ray_vertex[i].m_sigma = ssig;
						};
						rvv(rbase + 0, sx0 + rnx * rpad, sy0 + rny * rpad, a0,  rpad);
						rvv(rbase + 1, sx1 + rnx * rpad, sy1 + rny * rpad, a1,  rpad);
						rvv(rbase + 2, sx1 - rnx * rpad, sy1 - rny * rpad, a1, -rpad);
						rvv(rbase + 3, sx0 + rnx * rpad, sy0 + rny * rpad, a0,  rpad);
						rvv(rbase + 4, sx1 - rnx * rpad, sy1 - rny * rpad, a1, -rpad);
						rvv(rbase + 5, sx0 - rnx * rpad, sy0 - rny * rpad, a0, -rpad);
					}
				}
			}
		}
		return;
	}

	// End caps: gaussian dots driven by the same line_cap sliders as the classic path
	// (size/min/intensity-curve/brightness). The erf already gives the physical 50% end
	// roll-off; these add the visible bright endpoint on top, until the dwell-time model
	// (vertex_dwell) replaces them. In deflection mode the body uses DEFL_NOUT quads, so the
	// caps move to the slots right after it.
	// cap_no_persist: caps live in the separate NO-PERSIST FBO (post-pool, no afterimage) instead
	// of the core. That buffer's layout is fixed: slot [0] = no-persist dot (point path only, unused
	// here), [6] = cap0, [12] = cap1. When off, caps stay in the core buffer at their usual offsets.
	const bool caps_to_np = m_caps_glow && np_vertex != nullptr;
	const int cap0 = caps_to_np ? 6 : (m_defl_on ? DEFL_NOUT * 6 : 6);
	const int cap1 = caps_to_np ? 12 : (cap0 + 6);
	AnalyticLineVertex *const cap_tgt = caps_to_np ? np_vertex : vertex;
	// line path never uses the no-persist dot slot [0]; blank it so a stale value is not drawn
	if (np_vertex != nullptr) set_degenerate(np_vertex, 0);
	// Skip caps only when cap_no_persist is on but the np buffer could not be allocated this frame:
	// the core buffer has no cap slots then (verts_per_line dropped by 12), so writing would overrun.
	const bool cap_slots_ok = !(m_caps_glow && np_vertex == nullptr);
	if (cap_tgt != nullptr && cap0 >= 0 && cap_slots_ok)
	{
		// End caps: a beam-spot dot at each true endpoint. Its size is intrinsic - the same gaussian
		// sigma as the line cross-section (the dwelling beam spot), so it tracks the unified width
		// transfer automatically (a dim / thin line gets a small cap) and needs no separate size
		// sliders. line_cap_brightness sets the overall strength; vertex_dwell's per-endpoint factor
		// (start_cap / end_cap from the neighbour-aware pre-pass) modulates it - 1.0 at stroke termini
		// and sharp corners where the beam dwells, toward 0 at straight joints. The erf already gives
		// the physical 50% end roll-off; the cap adds the bright endpoint on top, scaled to ~0.5x so the
		// endpoint peak (0.5 erf + 0.5 cap) lands near the line intensity at full strength.
		const float cap_bright = std::max(0.0f, m_vs.line_cap_brightness);
		if (cap_bright > 0.0f)
		{
			// The cap is the finite blanking/Z transition of the ordinary moving spot. Derive it
			// before overload-width lift, overload bloom, edge defocus and HV droop.
			const float cap_over = sigmoid_width
				? overload_width_amount
				: std::clamp((n - ov_thresh) / ov_span * (as_point ? ov_dot : 1.0f), 0.0f, 1.0f);
			const float cap_smooth = cap_over * cap_over * (3.0f - 2.0f * cap_over);
			const float cap_scale = 0.5f * cap_bright * (1.0f - cap_smooth);
			// line_cap_width fattens the endpoint dot a touch beyond the bare beam spot (1.0 = exactly
			// the line's sigma); the dwell dot is usually a little larger than the running stroke.
			float sg_cap = std::max(sig_floor, normal_width / 3.2f);
			float cap_wcore = 0.0f;
			if (flat_f > 0.0f)
			{
				cap_wcore = flat_f * 0.5f * normal_width;
				sg_cap = std::max(sig_floor, sg_cap * (1.0f - flat_f));
			}
			sg_cap *= std::max(0.1f, m_vs.line_cap_width);
			const float cap_max = m_vs.line_cap_max_size * (m_vec_res_w / 1920.0f);
			if (cap_max > 0.0f)
			{
				const float extent = cap_wcore + sg_cap;
				if (extent > cap_max)
				{
					const float shrink = cap_max / extent;
					cap_wcore *= shrink;
					sg_cap *= shrink;
				}
			}
			auto cap_rgba_for = [&](float boost) -> uint32_t {
				const float s = cap_scale * boost;
				return u32Color(
					std::min<uint32_t>(uint32_t(prim->color.r * length_factor * s * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(prim->color.g * length_factor * s * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(prim->color.b * length_factor * s * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(std::clamp(display_a, 0.0f, 1.0f) * 255.0f + 0.5f), 255));
			};
			// No brightness compensation: the dedicated no-persist FBO is combined 1:1 (the JSON's
			// NoPersist Combine pass cancels the shader BLOOM_BRIGHTNESS_GAIN with scale 0.3745), so the
			// cap carries the same z as the core spot (core_over) whether it went to the core or np FBO.
			const float cap_z = core_over;
			set_dot(cap_tgt, cap0, x0, y0, sg_cap, cap_rgba_for(start_cap), cap_z, cap_wcore);
			set_dot(cap_tgt, cap1, x1, y1, sg_cap, cap_rgba_for(end_cap), cap_z, cap_wcore);
		}
		else
		{
			set_degenerate(cap_tgt, cap0);
			set_degenerate(cap_tgt, cap1);
		}
	}

	if (!m_defl_on)
	{
		// expanded quad: +-pad beyond both endpoints and to both sides
		const float sx0 = x0 - dx * pad, sy0 = y0 - dy * pad;   // start edge centre
		const float sx1 = x1 + dx * pad, sy1 = y1 + dy * pad;   // end edge centre
		const float a0 = -pad, a1 = seg_len + pad;
		// corners: 0=start+n, 1=end+n, 2=end-n, 3=start-n
		setv(0, sx0 + nx * pad, sy0 + ny * pad, a0, a0 - seg_len,  pad, sigma);
		setv(1, sx1 + nx * pad, sy1 + ny * pad, a1, a1 - seg_len,  pad, sigma);
		setv(2, sx1 - nx * pad, sy1 - ny * pad, a1, a1 - seg_len, -pad, sigma);
		setv(3, sx0 + nx * pad, sy0 + ny * pad, a0, a0 - seg_len,  pad, sigma);
		setv(4, sx1 - nx * pad, sy1 - ny * pad, a1, a1 - seg_len, -pad, sigma);
		setv(5, sx0 - nx * pad, sy0 - ny * pad, a0, a0 - seg_len, -pad, sigma);
	}
	else
	{
	// Deflection dynamics: draw the simulated beam trajectory as a short polyline of DEFL_NOUT sub-
	// quads. Each sub-quad uses a saturated axial term (a = +BIG, b = -BIG -> the erf difference is 1),
	// so there is no per-sub-segment end roll-off and the pieces join continuously; the true endpoints
	// are handled by the cap dots above. The perpendicular gaussian (d = +-pad) still gives the width.
	float tx[DEFL_NOUT + 1], ty[DEFL_NOUT + 1];
	const double draw_secs = (prim->t0 >= 0.0 && prim->t1 > prim->t0) ? (prim->t1 - prim->t0) : 0.0;
	simulate_deflection(x0, y0, x1, y1, draw_secs, tx, ty);
	// Only the true segment ends roll off (erf), so they stay round like the straight path; the
	// interior sub-quad joints use a saturated axial term (a=+BIG, b=-BIG -> axial 1) and join
	// seamlessly. Without this every sub-quad ended square, which made corners and hook tips boxy.
	const float BIG = 1.0e4f;
	for (int s = 0; s < DEFL_NOUT; s++)
	{
		const int idx = s * 6;
		const bool first = (s == 0), last = (s == DEFL_NOUT - 1);
		float ux = tx[s + 1] - tx[s], uy = ty[s + 1] - ty[s];
		const float sub_len = sqrtf(ux * ux + uy * uy);
		if (sub_len > 1e-4f) { ux /= sub_len; uy /= sub_len; } else { ux = dx; uy = dy; }
		const float pnx = uy, pny = -ux;   // sub-segment normal
		// back / front edge centres; extend past the true ends by pad so the roll-off has area
		float bx = tx[s],     by = ty[s];
		float fx = tx[s + 1], fy = ty[s + 1];
		if (first) { bx -= ux * pad; by -= uy * pad; }
		if (last)  { fx += ux * pad; fy += uy * pad; }
		// axial coords: a measured from this segment's start (roll off at first sub-quad's back),
		// b from its end (roll off at last sub-quad's front); BIG elsewhere = no joint roll-off
		const float aB = first ? -pad     : BIG;
		const float aF = first ? sub_len  : BIG;
		const float bB = last  ? -sub_len : -BIG;
		const float bF = last  ? pad      : -BIG;
		setv(idx + 0, bx + pnx * pad, by + pny * pad, aB, bB,  pad, sigma);
		setv(idx + 1, fx + pnx * pad, fy + pny * pad, aF, bF,  pad, sigma);
		setv(idx + 2, fx - pnx * pad, fy - pny * pad, aF, bF, -pad, sigma);
		setv(idx + 3, bx + pnx * pad, by + pny * pad, aB, bB,  pad, sigma);
		setv(idx + 4, fx - pnx * pad, fy - pny * pad, aF, bF, -pad, sigma);
		setv(idx + 5, bx - pnx * pad, by - pny * pad, aB, bB, -pad, sigma);
	}
	}

	// Analytic glow: the line widened into a low-amplitude gaussian along the straight chord (in the
	// slots after the body + caps), added into the FBO. Wide sigma -> broad soft tail; accumulates
	// across lines into the scene glow, tracking the beam exactly with no pyramid and no temporal lag.
	if (glow_vertex)
	{
		if (m_glow_off_glow >= 0)
		{
		const float gpad = 3.5f * glow_sig + 0.5f;
		const float gsx0 = x0 - dx * gpad, gsy0 = y0 - dy * gpad;
		const float gsx1 = x1 + dx * gpad, gsy1 = y1 + dy * gpad;
		const float ga0 = -gpad, ga1 = seg_len + gpad;
		auto gv = [&](int i, float x, float y, float a, float b, float d) {
			glow_vertex[i].m_x = x; glow_vertex[i].m_y = y; glow_vertex[i].m_z = 0.0f; glow_vertex[i].m_rgba = glow_rgba;
			glow_vertex[i].m_u = 0.0f; glow_vertex[i].m_v = 0.0f;
			glow_vertex[i].m_a = a; glow_vertex[i].m_b = b; glow_vertex[i].m_d = d; glow_vertex[i].m_sigma = glow_sig;
		};
		gv(m_glow_off_glow + 0, gsx0 + nx * gpad, gsy0 + ny * gpad, ga0, ga0 - seg_len,  gpad);
		gv(m_glow_off_glow + 1, gsx1 + nx * gpad, gsy1 + ny * gpad, ga1, ga1 - seg_len,  gpad);
		gv(m_glow_off_glow + 2, gsx1 - nx * gpad, gsy1 - ny * gpad, ga1, ga1 - seg_len, -gpad);
		gv(m_glow_off_glow + 3, gsx0 + nx * gpad, gsy0 + ny * gpad, ga0, ga0 - seg_len,  gpad);
		gv(m_glow_off_glow + 4, gsx1 - nx * gpad, gsy1 - ny * gpad, ga1, ga1 - seg_len, -gpad);
		gv(m_glow_off_glow + 5, gsx0 - nx * gpad, gsy0 - ny * gpad, ga0, ga0 - seg_len, -gpad);
		}
		// Overdrive white flare (slots 18-23): an overdriven line blooms white-hot here in the glow buffer
		// (post-mask), at the beam's own sigma (not the wide analytic glow), so it is not mask-patterned.
		if (m_glow_off_flare >= 0 && flare_on)
		{
			const float fpad = wcore + 3.5f * sigma + 0.5f;
			const float fsx0 = x0 - dx * fpad, fsy0 = y0 - dy * fpad;
			const float fsx1 = x1 + dx * fpad, fsy1 = y1 + dy * fpad;
			const float fa0 = -fpad, fa1 = seg_len + fpad;
			auto fv = [&](int i, float x, float y, float a, float b, float d) {
				glow_vertex[i].m_x = x; glow_vertex[i].m_y = y; glow_vertex[i].m_z = flare_z; glow_vertex[i].m_rgba = flare_rgba;
				glow_vertex[i].m_u = wcore; glow_vertex[i].m_v = 0.0f;
				glow_vertex[i].m_a = a; glow_vertex[i].m_b = b; glow_vertex[i].m_d = d; glow_vertex[i].m_sigma = sigma;
			};
			fv(m_glow_off_flare + 0, fsx0 + nx * fpad, fsy0 + ny * fpad, fa0, fa0 - seg_len,  fpad);
			fv(m_glow_off_flare + 1, fsx1 + nx * fpad, fsy1 + ny * fpad, fa1, fa1 - seg_len,  fpad);
			fv(m_glow_off_flare + 2, fsx1 - nx * fpad, fsy1 - ny * fpad, fa1, fa1 - seg_len, -fpad);
			fv(m_glow_off_flare + 3, fsx0 + nx * fpad, fsy0 + ny * fpad, fa0, fa0 - seg_len,  fpad);
			fv(m_glow_off_flare + 4, fsx1 - nx * fpad, fsy1 - ny * fpad, fa1, fa1 - seg_len, -fpad);
			fv(m_glow_off_flare + 5, fsx0 - nx * fpad, fsy0 - ny * fpad, fa0, fa0 - seg_len, -fpad);
		}
		else if (glow_vertex && m_glow_off_flare >= 0)
			set_degenerate(glow_vertex, m_glow_off_flare);
		// Overload Glow (bloom): same wide-halo mechanism as the point path above.
		if (m_glow_off_oglow >= 0 && oglow_on)
		{
			const float opad = 3.5f * oglow_sig + 0.5f;
			const float osx0 = x0 - dx * opad, osy0 = y0 - dy * opad;
			const float osx1 = x1 + dx * opad, osy1 = y1 + dy * opad;
			const float oa0 = -opad, oa1 = seg_len + opad;
			auto ov = [&](int i, float x, float y, float a, float b, float d) {
				glow_vertex[i].m_x = x; glow_vertex[i].m_y = y; glow_vertex[i].m_z = oglow_z; glow_vertex[i].m_rgba = oglow_rgba;
				glow_vertex[i].m_u = 0.0f; glow_vertex[i].m_v = 0.0f;
				glow_vertex[i].m_a = a; glow_vertex[i].m_b = b; glow_vertex[i].m_d = d; glow_vertex[i].m_sigma = oglow_sig;
			};
			ov(m_glow_off_oglow + 0, osx0 + nx * opad, osy0 + ny * opad, oa0, oa0 - seg_len,  opad);
			ov(m_glow_off_oglow + 1, osx1 + nx * opad, osy1 + ny * opad, oa1, oa1 - seg_len,  opad);
			ov(m_glow_off_oglow + 2, osx1 - nx * opad, osy1 - ny * opad, oa1, oa1 - seg_len, -opad);
			ov(m_glow_off_oglow + 3, osx0 + nx * opad, osy0 + ny * opad, oa0, oa0 - seg_len,  opad);
			ov(m_glow_off_oglow + 4, osx1 - nx * opad, osy1 - ny * opad, oa1, oa1 - seg_len, -opad);
			ov(m_glow_off_oglow + 5, osx0 - nx * opad, osy0 - ny * opad, oa0, oa0 - seg_len, -opad);
		}
		else if (glow_vertex && m_glow_off_oglow >= 0)
			set_degenerate(glow_vertex, m_glow_off_oglow);
	}
	// Lines do not emit point-only halation geometry, but their reserved optical slots
	// must still be initialized because the optical buffer is packed per visible vector.
	AnalyticLineVertex *const optics_vertex = m_optical_separate ? optical_vertex : glow_vertex;
	if (optics_vertex)
	{
		if (m_glow_off_ring >= 0) set_degenerate(optics_vertex, m_glow_off_ring);
		if (m_glow_off_fill >= 0) set_degenerate(optics_vertex, m_glow_off_fill);
	}
}

void renderer_bgfx::put_solid_line(render_primitive *prim, ScreenVertex* vertex)
{
	float x0 = prim->bounds.x0;
	float y0 = prim->bounds.y0;
	float x1 = prim->bounds.x1;
	float y1 = prim->bounds.y1;

	float dx = x1 - x0;
	float dy = y1 - y0;
	const float seg_len = sqrtf(dx * dx + dy * dy);

	const float point_threshold = m_vs.line_point_threshold;
	// Point-treatment test: short segments (add_point gives x0==x1,y0==y1 -> seg_len 0) are drawn as a
	// single circle so the two half-circle caps do not overlap into a double-bright distorted blob.
	const bool as_point = (seg_len <= point_threshold);

	// Unified per-vector transfers (see put_analytic_line for the full rationale). drive = beam_energy
	// when the device supplies it, else the display intensity. Brightness and width are two independent
	// clipped power curves out = clamp((drive-lo)/(hi-lo),0,1)^gamma, decoupled so brightness can
	// saturate while width keeps growing - this replaces the old renderer-side overload model.
	// n = the normalized beam energy; n > 1 is the genuine overload that drives the white flare/bloom.
	// (The legacy beam_energy_ref divisor is gone - the unified energy model's energy_speed_norm and
	// overload_threshold cover its role.) When the device supplies NO beam_energy (< 0) the renderer
	// derives it from the per-segment timestamps (unified model, generic_beam_energy); with energy_model
	// off that reduces to the plain display intensity clamp(color.a) = the prior behaviour.
	// Speed-normalisation basis = the vector CONTENT width (same basis as every px magnitude), NOT
	// the window: a portrait game pillarboxed in a wide window would otherwise read its beam speeds
	// ~2x too slow (window-relative lengths) and everything would run hot.
	const float e_screen_ref = (m_vec_res_w > 1.0f) ? m_vec_res_w
			: float(std::max(s_width[window().index()], s_height[window().index()]));
	float n = (prim->beam_energy >= 0.0f) ? prim->beam_energy
										  : generic_beam_energy(prim, seg_len, as_point, e_screen_ref);
	// Energy Jitter (near-saturation shimmer): a vector whose normalized energy n approaches
	// saturation wobbles by a band-limited PER-VECTOR random factor; dim vectors are untouched
	// (the weight hits 0 at the onset), unlike the retired whole-frame Vector Flicker. Applied to n
	// itself, BEFORE the transfer/overload chain, so below the two-regime threshold the brightness
	// wobbles, and above it the core stays saturated while width / white-pull / flare / Overload
	// Glow / halation shimmer - a bright beam that stays bright but trembles. The time axis is
	// emulated time quantized to energy_jitter_hz steps with smoothstep value-noise between steps
	// (bounded speed, freezes on pause); the per-vector seed hashes the quantized endpoints, so the
	// wobble is stable within a frame and independent between vectors, with no RNG state.
	const float jit = m_vs.energy_jitter;
	if (jit > 0.0f)
	{
		const float j_onset = m_vs.energy_jitter_onset;
		const float j_ramp  = std::max(0.05f, m_vs.energy_jitter_ramp);
		// Base floor: normal (below-onset) vectors still get a slight always-on wobble (analog-noise
		// shimmer of the whole image); the near-saturation ramp adds on top. energy_jitter_base 0 =
		// ramp only (prior behaviour - only near-saturation vectors wobble).
		const float j_w = std::max(std::clamp(m_vs.energy_jitter_base, 0.0f, 1.0f),
								   std::clamp((n - j_onset) / j_ramp, 0.0f, 1.0f));
		if (j_w > 0.0f)
		{
			const float j_hz = std::max(1.0f, m_vs.energy_jitter_hz);
			const double j_t = m_vec_time_ms * double(j_hz) * 0.001;
			const uint32_t j_step = uint32_t(int64_t(j_t));
			const float j_frac = float(j_t - double(j_step));
			auto jhash = [](uint32_t a) { a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16; return a; };
			const uint32_t j_seed = jhash(uint32_t(int32_t(x0 * 8.0f)) * 0x9e3779b9U)
								  ^ jhash(uint32_t(int32_t(y0 * 8.0f)) + 0x85ebca6bU)
								  ^ jhash(uint32_t(int32_t(x1 * 8.0f)) + 0xc2b2ae35U)
								  ^ jhash(uint32_t(int32_t(y1 * 8.0f)) + 0x27d4eb2fU);
			const float j_r0 = float(jhash(j_seed ^ jhash(j_step))      & 0xffffffu) / float(0x800000) - 1.0f;
			const float j_r1 = float(jhash(j_seed ^ jhash(j_step + 1u)) & 0xffffffu) / float(0x800000) - 1.0f;
			const float j_sm = j_frac * j_frac * (3.0f - 2.0f * j_frac);
			n *= std::max(0.0f, 1.0f + jit * j_w * (j_r0 + (j_r1 - j_r0) * j_sm));
		}
	}
	const float drive = std::clamp(n, 0.0f, 1.0f);
	// Stock fallback (chains without bright_threshold, e.g. default-vector): plain intensity-linear
	// response. The legacy intensity_clip_* / width_clip_* / intensity_curve transfer knobs are gone
	// (their last user, vector-vectrex-3d.json, was retired with the legacy chains); the two-regime
	// two-regime transfer below overrides both values on every phosphor chain.
	float display_a = drive;
	float wf = drive;
	// Two-regime transfer (bright_threshold > 0 enables it; 0 = stock, other chains unaffected):
	// brightness rises to max at the threshold T then SATURATES; energy above T is poured into the WIDTH
	// instead (a gentle width slope below T, a steep one above). width_knee = the width fraction reached
	// at T. b is the normalized beam energy (drive). This overrides display_a and wf computed above.
	const float bright_thresh = m_vs.bright_threshold;
	if (bright_thresh > 0.0f)
	{
		const float T = std::min(bright_thresh, 0.999f);
		display_a = std::clamp(drive / T, 0.0f, 1.0f);                      // brightness from clamped drive, saturates at T
		// Width uses the UNCLAMPED energy n (which can exceed 1), so once brightness saturates the beam
		// keeps getting thicker as energy rises (no width ceiling at n=1). bw_max = width at n=1.
		const float below = std::min(n, T) / T;                            // 0..1 over [0,T]
		// Width growth beyond beam_width_max once the beam is driven past the threshold (e.g. an object-lifted
		// bullet/explosion). The cap is the beam_width_overmax slider (multiple of the bw_min->bw_max span that the
		// "above" region can add): raise it so "lifted" objects get several times the normal width. Default 4.
		const float w_overmax = std::max(0.0f, m_vs.beam_width_overmax);
		float above = (n > T) ? std::min((n - T) / std::max(0.05f, 1.0f - T), w_overmax) : 0.0f;
		// width_over_curve shapes the overload-lift region ("above", the growth beyond beam_width_max)
		// the same way width_curve shapes the sub-threshold ramp below - width_curve deliberately excludes
		// this region (see the "leave the >1 region ... linear" note below), so without this the beam's
		// thickness always grew linearly with overload no matter how the OTHER overload-driven effects
		// (colour saturation, Overload Glow) were curved. pow(above/w_overmax, curve)*w_overmax keeps 0
		// and w_overmax fixed (only bends the path between), matching the wcurve convention.
		const float wocurve = m_vs.width_over_curve;
		if (wocurve != 1.0f && above > 0.0f && w_overmax > 0.0f) above = powf(above / w_overmax, wocurve) * w_overmax;
		const float knee  = std::clamp(m_vs.width_knee, 0.0f, 1.0f);
		wf = knee * below + (1.0f - knee) * above;                          // gentle below T, steep above (unbounded)
		// Optional shaping curves (powers) on top of the linear ramps: bright_curve bends the
		// energy->brightness response (>1 = darker mids / later rise to peak, <1 = brighter mids);
		// width_curve bends the energy->width response the same way. 1.0 = linear (unchanged).
		const float bcurve = m_vs.bright_curve;
		if (bcurve != 1.0f) display_a = powf(std::clamp(display_a, 0.0f, 1.0f), bcurve);
		const float wcurve = m_vs.width_curve;
		// Shape ONLY the min->max transition (wf in [0,1]); leave the >1 region (energy past the
		// threshold, where the beam grows beyond beam_width_max) linear. pow keeps 0->0 and 1->1, so
		// bw_min and bw_max stay fixed and the curve only bends the path between them - it must not
		// rescale the max itself.
		if (wcurve != 1.0f && wf > 0.0f && wf < 1.0f) wf = powf(wf, wcurve);
		// Optional sigmoid (S-curve) shaping ON TOP of the power curves, for a snappier CRT-like
		// brightness/width transition. bright_sigmoid/width_sigmoid > 0 = sigmoid (sharper contrast),
		// < 0 = softer (ease); 0 = off. *_sigmoid_center sets the inflection. Width keeps 0->0 / 1->1.
		const float bsig = m_vs.bright_sigmoid;
		if (bsig != 0.0f) display_a = vec_scurve(display_a, bsig, m_vs.bright_sigmoid_center);
		const float wsig = m_vs.width_sigmoid;
		if (wsig != 0.0f && wf > 0.0f && wf < 1.0f) wf = vec_scurve(wf, wsig, m_vs.width_sigmoid_center);
		// Normal-brightness cap, released when the beam is "lifted" (driven past the ref, n>1 = an
		// object-lifted bullet/explosion). Brightness is clamped to bright_normal_cap for normal beams and
		// the cap ramps back to 1.0 as n goes 1->2, so ordinary objects sit dimmer while lifted ones reach
		// full white. 1.0 = off (no cap).
		const float bcap = m_vs.bright_normal_cap;
		if (bcap < 1.0f)
		{
			const float uncap = std::clamp(n - 1.0f, 0.0f, 1.0f);   // 0 = normal (n<=1), 1 = lifted (n>=2)
			display_a = std::min(display_a, bcap + (1.0f - bcap) * uncap);
		}
	}
	const float bw_min = m_vs.beam_width_min;
	const float bw_max = m_vs.beam_width_max;
	// Normal range maps wf 0..1 to bw_min..bw_max. The LIFT beyond bw_max (wf>1, e.g. an object-lifted
	// bullet/explosion) is added as a multiple of bw_max, NOT of the (bw_max-bw_min) span - so a small
	// min/max gap no longer shrinks the lift. For wf>1 this reduces exactly to width = bw_max * wf.
	float beam_units = bw_min + std::min(wf, 1.0f) * (bw_max - bw_min);
	// beam_width_over_scale lets the overload-lift term (wf>1, i.e. n past bright_threshold) use its
	// OWN scale instead of reusing bw_max - so the overload maximum can be raised independently of the
	// normal (non-overloaded) range's own width, which bw_max alone also governs via the "below" blend
	// above. -1 (default) = inherit bw_max, reproducing the previous behaviour exactly.
	const float w_over_scale_slider = m_vs.beam_width_over_scale;
	const float w_over_scale = (w_over_scale_slider >= 0.0f) ? w_over_scale_slider : bw_max;
	if (wf > 1.0f) beam_units += (wf - 1.0f) * w_over_scale;
	// point_width_scale: plain size multiplier for every dot (the analytic path additionally has
	// the junction-dot scale; this legacy path keeps the simple behaviour).
	if (as_point) beam_units *= m_vs.point_width_scale;
	// beam_units are pixel widths at a 1920px-wide window; scale to the current resolution.
	float width = beam_units * (m_vec_res_w / 1920.0f);
	const float ovld = 0.0f;  // overload model removed; the width transfer handles beam widening
	if (width < 0.5f) width = 0.5f;

	// The legacy length-fade and dot_boost knobs are gone (superseded by the unified energy model
	// on the analytic path).
	float length_factor = 1.0f;

	// Pack the line color: hue from the primitive, alpha = display intensity.
	const uint32_t rgba = u32Color(
		uint32_t(prim->color.r * length_factor * 255.0f + 0.5f),
		uint32_t(prim->color.g * length_factor * 255.0f + 0.5f),
		uint32_t(prim->color.b * length_factor * 255.0f + 0.5f),
		uint32_t(std::clamp(display_a, 0.0f, 1.0f) * 255.0f + 0.5f));

	if (seg_len > 0.0001f)
	{
		const float inv = 1.0f / seg_len;
		dx *= inv;
		dy *= inv;
	}
	else
	{
		dx = dy = 0.70710678f;
	}

	// normal perpendicular to the line direction (normalized)
	float nx = dy;
	float ny = -dx;

	// widen the quad by 1 pixel to accommodate the AA fade
	float r = width * 0.5f + 0.5f;
	if (r < 1.0f) r = 1.0f;

	// The line body is an (x0..x1) quad with no end extension; the rounded fans handle the ends,
	// so the cap/body boundary is continuous.

	// line-body quad, 4 vertices (indices: 0=start+n, 1=end+n, 2=end-n, 3=start-n)
	const float qx[4] = { x0 + nx * r, x1 + nx * r, x1 - nx * r, x0 - nx * r };
	const float qy[4] = { y0 + ny * r, y1 + ny * r, y1 - ny * r, y0 - ny * r };
	const float qv[4] = { 0.0f,        0.0f,        1.0f,        1.0f        };

	// Cap center vertex color. The Line Cap Brightness slider scales the cap relative to the body:
	// >1 makes it glow brighter, <1 dims it (the cap fan replaces the end, so this is its intensity).
	const float cap_bright = std::max(0.0f, m_vs.line_cap_brightness);
	uint32_t cap_center_rgba = rgba;
	if (cap_bright < 0.9999f || cap_bright > 1.0001f)
	{
		const uint32_t r8 = std::min<uint32_t>(uint32_t(prim->color.r * length_factor * cap_bright * 255.0f + 0.5f), 255);
		const uint32_t g8 = std::min<uint32_t>(uint32_t(prim->color.g * length_factor * cap_bright * 255.0f + 0.5f), 255);
		const uint32_t b8 = std::min<uint32_t>(uint32_t(prim->color.b * length_factor * cap_bright * 255.0f + 0.5f), 255);
		const uint32_t a8 = std::min<uint32_t>(uint32_t(std::clamp(display_a, 0.0f, 1.0f) * 255.0f + 0.5f), 255);
		cap_center_rgba = u32Color(r8, g8, b8, a8);
	}

	auto setv = [&](int i, float x, float y, float v_value, uint32_t vrgba = 0) {
		vertex[i].m_x = x;
		vertex[i].m_y = y;
		vertex[i].m_z = 0.0f;
		vertex[i].m_rgba = (vrgba == 0) ? rgba : vrgba;
		vertex[i].m_u = ovld;     // per-line overload amount (fs_vector_line blends parabola<->Gaussian)
		vertex[i].m_v = v_value;  // across the line width [0..1], 0.5 = center
	};

	// Cap radius in pixels, scaled to resolution against a 1920px-wide base so the on-screen cap
	// size stays constant across window resolutions. The cap only expands the line when it exceeds
	// the line-body radius r; thicker lines (r >= cap radius) get a plain rounded end.
	const float cap_res_scale = m_vec_res_w / 1920.0f;
	// Cap radius interpolates line_cap_min_size..line_cap_size by line intensity (prim->color.a) via
	// the Line Cap Intensity Curve (pow exponent); curve 0 (default) keeps the full size for every line.
	const float cap_full   = m_vs.line_cap_size * cap_res_scale;
	const float cap_min_px = m_vs.line_cap_min_size * cap_res_scale;
	const float cap_curve  = m_vs.line_cap_intensity_curve;
	const float cap_bi     = std::clamp(prim->color.a, 0.0f, 1.0f);
	const float cap_f      = (cap_curve <= 0.0001f) ? 1.0f : powf(cap_bi, cap_curve);
	const float fixed_cap_radius = std::max(0.0f, cap_min_px + (cap_full - cap_min_px) * cap_f);
	const float cap_extent       = std::max(0.0f, fixed_cap_radius - r);

	int vi = 6;

	if (as_point)
	{
		// point-treatment branch
		// Skip the body (fill the 6 body vertices as degenerate triangles at the center) and spend
		// the two end caps' vertex budget (48) on a single 360-degree fan. The caps no longer
		// overlap, drawing one clean circle for the point.
		const float cx = (x0 + x1) * 0.5f;
		const float cy = (y0 + y1) * 0.5f;

		// line-body slots 0-5: degenerate at the center point (zero-area, culled by the GPU)
		for (int i = 0; i < 6; ++i)
			setv(i, cx, cy, 0.5f, cap_center_rgba);

		// full-circle fan: LINE_CAP_SEGMENTS * 2 = 16 segments, 48 vertices
		const int full_segs = LINE_CAP_SEGMENTS * 2;
		const float dtheta_full = 2.0f * 3.14159265f / float(full_segs);
		const float pr = std::max(r, fixed_cap_radius);  // ensure the point is at least the fixed radius
		for (int i = 0; i < full_segs; ++i)
		{
			const float t0 = float(i) * dtheta_full;
			const float t1 = float(i + 1) * dtheta_full;
			const float p0x = cx + pr * cosf(t0);
			const float p0y = cy + pr * sinf(t0);
			const float p1x = cx + pr * cosf(t1);
			const float p1y = cy + pr * sinf(t1);

			setv(vi++, cx,  cy,  0.5f, cap_center_rgba);  // center: boosted color, fade=1
			setv(vi++, p0x, p0y, 0.0f);                   // rim, fade=0
			setv(vi++, p1x, p1y, 0.0f);
		}
	}
	else
	{
		// line body: triangle 1 (0,1,2), triangle 2 (0,2,3)
		setv(0, qx[0], qy[0], qv[0]);
		setv(1, qx[1], qy[1], qv[1]);
		setv(2, qx[2], qy[2], qv[2]);
		setv(3, qx[0], qy[0], qv[0]);
		setv(4, qx[2], qy[2], qv[2]);
		setv(5, qx[3], qy[3], qv[3]);

		// rounded end-cap fan
		// radius r(theta) = r + cap_extent * sin theta:
		//   theta=0    -> r              (continuous with the line-body corner)
		//   theta=pi/2 -> r + cap_extent (= fixed_cap_radius if cap > r, else r for a plain rounded line)
		//   theta=pi   -> r              (continuous with the opposite corner)
		// On thin lines, cap_extent > 0 swells the circle into a "dot".
		// On thick lines (r >= fixed_cap_radius), cap_extent=0 gives a plain half-circle = rounded line.
		const float dtheta = 3.14159265f / float(LINE_CAP_SEGMENTS);

		auto add_cap = [&](float cx, float cy, float ax, float ay) {
			for (int i = 0; i < LINE_CAP_SEGMENTS; ++i)
			{
				const float t0 = float(i) * dtheta;
				const float t1 = float(i + 1) * dtheta;
				const float c0 = cosf(t0), s0 = sinf(t0);
				const float c1 = cosf(t1), s1 = sinf(t1);
				// radii r0/r1 = r + cap_extent * sin theta; cap_extent is added only along the axis (theta=pi/2).
				const float r0 = r + cap_extent * s0;
				const float r1 = r + cap_extent * s1;

				const float p0x = cx + r0 * (c0 * nx + s0 * ax);
				const float p0y = cy + r0 * (c0 * ny + s0 * ay);
				const float p1x = cx + r1 * (c1 * nx + s1 * ax);
				const float p1y = cy + r1 * (c1 * ny + s1 * ay);

				// triangle: center (boosted color) -> p0 (normal color) -> p1
				setv(vi++, cx,  cy,  0.5f, cap_center_rgba);  // fade=1, boost
				setv(vi++, p0x, p0y, 0.0f);                   // fade=0, normal
				setv(vi++, p1x, p1y, 0.0f);                   // fade=0, normal
			}
		};

		add_cap(x0, y0, -dx, -dy);  // start cap (half-circle opposite the line direction)
		add_cap(x1, y1,  dx,  dy);  // end cap (half-circle along the line direction)
	}
}

int renderer_bgfx::draw(int update)
{
	int window_index = window().index();

	m_seen_views.clear();
	if (m_ortho_view)
		m_ortho_view->set_index(UINT_MAX);
	// HDR composite: reset the per-frame work-target view.
	m_hdr_work_view = UINT_MAX;

	osd_dim wdim = window().get_size_pixels();
	s_width[window_index] = wdim.width();
	s_height[window_index] = wdim.height();

	// Set view 0 default viewport.
	if (window_index == 0)
	{
		s_current_view = 0;
	}

	// Apply any chain-selection change requested via the UI slider here, at a clean frame boundary
	// before any render target is touched this frame. The reload destroys and recreates the chain's
	// bgfx targets; doing that inside the slider callback (mid-frame, with the previous frame's GPU
	// work possibly still in flight) races on Metal and corrupts the display until restart. No-op when
	// nothing is pending. See chain_manager::m_reload_pending.
	m_chains->process_pending_reload();

	window().m_primlist->acquire_lock();
	uint32_t num_screens = m_chains->update_screen_textures(s_current_view, window().m_primlist->first(), window());
	window().m_primlist->release_lock();

	bool skip_frame = update_dimensions();
	if (skip_frame)
	{
		return 0;
	}

	// Per-frame refresh of the vector slider cache (screen 0 chain; harmless no-op defaults
	// when no chain is active). Must precede everything that reads m_vs.
	if (window_index == 0)
	{
		refresh_vec_slider_cache();
		// Chain-driven opt-in for the analytic vector engine (see m_vec_engine_active): only a
		// chain declaring "vector_engine": "analytic" routes vector LINEs through the FBO path.
		bgfx_chain *const vchain = (m_chains != nullptr) ? m_chains->screen_chain(0) : nullptr;
		m_vec_engine_active = (vchain != nullptr) && vchain->vector_engine();
	}

	// All sliders come from the chain JSON and are restored via chain_manager's standard load_config.

	if (num_screens)
	{
		// Restore config after counting screens the first time
		// Doing this here is hacky - it means config is restored at the wrong
		// time if the initial view has no screens and the user switches to a
		// view with screens.  The trouble is there's no real interface between
		// the render targets and the renderer so we don't actually know when
		// we're first called on to render a live view (as opposed to an info
		// screen).
		if (m_config)
		{
			osd_printf_verbose("BGFX: Applying configuration for window %d\n", window().index());
			m_chains->load_config(*m_config->get_first_child());
			m_config.reset();
		}

		uint32_t chain_view_count = m_chains->process_screen_chains(s_current_view, window());
		s_current_view += chain_view_count;
	}

	if (s_current_view > m_max_view)
	{
		m_max_view = s_current_view;
	}
	else
	{
		s_current_view = m_max_view;
	}

	window().m_primlist->acquire_lock();

	// Mark our texture atlas as dirty if we need to do so
	bool atlas_valid = update_atlas();

	// Layer the UI on top of the vector blit. Difference from stock: using view 0 for both the UI
	// and the clear would ADD-composite the vector blit over the UI, hiding/ghosting it. So view 0
	// is used for a manual clear only, and the UI's m_ortho_view is allocated at a late index in
	// buffer_primitives and drawn last.
	if (window_index == 0)
	{
		const uint16_t clear_view = uint16_t(s_current_view++);
		const uint16_t cw = uint16_t(s_width[window_index]);
		const uint16_t ch = uint16_t(s_height[window_index]);
		bgfx::setViewFrameBuffer(clear_view, BGFX_INVALID_HANDLE);
		bgfx::setViewRect(clear_view, 0, 0, cw, ch);
		bgfx::setViewClear(clear_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
		bgfx::setViewMode(clear_view, bgfx::ViewMode::Sequential);
		bgfx::touch(clear_view);

	}

	// Release the vector FBOs while a chain without the analytic-engine opt-in is active: the
	// stock buffer_primitives path then draws vector LINEs untouched, with no idle GPU resources
	// left behind. (bgfx::destroy is deferred to frame end, so in-flight views stay valid.)
	if (window_index == 0 && !m_vec_engine_active)
	{
		if (bgfx::isValid(m_vec_fb))      { bgfx::destroy(m_vec_fb);      m_vec_fb = BGFX_INVALID_HANDLE; }
		if (bgfx::isValid(m_vec_glow_fb)) { bgfx::destroy(m_vec_glow_fb); m_vec_glow_fb = BGFX_INVALID_HANDLE; }
		if (bgfx::isValid(m_vec_optical_fb)) { bgfx::destroy(m_vec_optical_fb); m_vec_optical_fb = BGFX_INVALID_HANDLE; }
		if (bgfx::isValid(m_vec_np_fb))   { bgfx::destroy(m_vec_np_fb);   m_vec_np_fb = BGFX_INVALID_HANDLE; }
		m_vec_fb_w = m_vec_fb_h = 0;
		m_vec_glow_fb_w = m_vec_glow_fb_h = 0;
	}

	// Create/recreate the vector FBOs while the analytic engine is active. Initial creation is
	// lazy (every size tracker starts at 0, so the first engine frame mismatches and creates);
	// a size or glow-scale mismatch after a resize or chain switch recreates the same way.
	if (window_index == 0 && m_vec_engine_active)
	{
		const uint16_t cur_w = uint16_t(s_width[window_index]);
		const uint16_t cur_h = uint16_t(s_height[window_index]);
		const uint16_t render_w = std::max<uint16_t>(1, uint16_t(float(cur_w) * m_vec_render_scale + 0.5f));
		const uint16_t render_h = std::max<uint16_t>(1, uint16_t(float(cur_h) * m_vec_render_scale + 0.5f));
		const uint16_t target_fb_w = uint16_t(render_w * m_vec_supersample);
		const uint16_t target_fb_h = uint16_t(render_h * m_vec_supersample);
		// glow_fbo_scale: the active chain's glow-FBO resolution factor (a fast-variant chain sets 0.5
		// to quarter the glow fill cost; chains without the slider get 1.0). The glow content is smooth
		// analytic gaussians computed from interpolated line-local varyings, so a reduced raster only
		// samples the same function at lower density. Tracked separately so a chain switch that changes
		// only this factor recreates the FBOs.
		const float glow_scale = std::clamp(m_chains->slider_value(0, "glow_fbo_scale", 1.0f), 0.1f, 1.0f);
		const uint16_t target_glow_w = std::max<uint16_t>(1, uint16_t(target_fb_w * glow_scale));
		const uint16_t target_glow_h = std::max<uint16_t>(1, uint16_t(target_fb_h * glow_scale));
		const bool optical_supported = m_chains->slider_value(0, "glow_tail_curve", -1.0f) >= 0.0f
			&& (m_chains->slider_value(0, "ring_gain", -1.0f) >= 0.0f
				|| m_chains->slider_value(0, "ring_fill", -1.0f) >= 0.0f
				|| m_chains->slider_value(0, "ray_gain", -1.0f) >= 0.0f);
		if (cur_w > 0 && cur_h > 0 && (target_fb_w != m_vec_fb_w || target_fb_h != m_vec_fb_h
			|| target_glow_w != m_vec_glow_fb_w || target_glow_h != m_vec_glow_fb_h
			|| optical_supported != bgfx::isValid(m_vec_optical_fb)))
		{
			if (bgfx::isValid(m_vec_fb))
				bgfx::destroy(m_vec_fb);
			if (bgfx::isValid(m_vec_glow_fb))
				bgfx::destroy(m_vec_glow_fb);
			if (bgfx::isValid(m_vec_optical_fb))
				bgfx::destroy(m_vec_optical_fb);
			if (bgfx::isValid(m_vec_np_fb))
				bgfx::destroy(m_vec_np_fb);
			m_vec_fb_w = target_fb_w;
			m_vec_fb_h = target_fb_h;
			m_vec_glow_fb_w = target_glow_w;
			m_vec_glow_fb_h = target_glow_h;
			osd_printf_verbose("BGFX: analytic vector internal %ux%u, raster FBO %ux%u, output %ux%u\n",
				render_w, render_h, m_vec_fb_w, m_vec_fb_h, cur_w, cur_h);
			// bilinear (no MSAA, for sampler compatibility)
			const uint64_t cf = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
				BGFX_TEXTURE_RT;
			bgfx::TextureHandle tc = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, cf);
			bgfx::TextureHandle td = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::D32F, cf);
			bgfx::TextureHandle at[2] = { tc, td };
			m_vec_fb = bgfx::createFrameBuffer(2, at, true);
			bgfx::TextureHandle gc = bgfx::createTexture2D(m_vec_glow_fb_w, m_vec_glow_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, cf);
			m_vec_glow_fb = bgfx::createFrameBuffer(1, &gc, true);
			if (optical_supported)
			{
				bgfx::TextureHandle oc = bgfx::createTexture2D(m_vec_glow_fb_w, m_vec_glow_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, cf);
				m_vec_optical_fb = bgfx::createFrameBuffer(1, &oc, true);
			}
			else
				m_vec_optical_fb = BGFX_INVALID_HANDLE;
			bgfx::TextureHandle npc = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, cf);
			m_vec_np_fb = bgfx::createFrameBuffer(1, &npc, true);
		}
	}

	// Chain bootstrap / keep-alive while the analytic engine is OFF: for a vector game the
	// chain-selection slider is created by update_screen_count(1), which otherwise only happens
	// inside the engine path's inject_vector_screen - without this, launching with a non-engine
	// chain active (or before the first chain loads) would never surface the chain selection at
	// all, so there would be no way to select an engine chain (the bootstrap deadlock). The chain
	// itself is NOT processed here: its texture providers may be stale while the engine is off,
	// and the stock buffer_primitives path draws the vector LINEs directly.
	if (window_index == 0 && !m_vec_engine_active && atlas_valid)
	{
		bool have_vectors = false;
		for (render_primitive *scan = window().m_primlist->first(); scan != nullptr; scan = scan->next())
			if (scan->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(scan->flags)) { have_vectors = true; break; }
		if (have_vectors)
		{
			m_chains->ensure_vector_screen_slot();
			// Restore slider values from config once, the first time, after the chain is loaded
			// (same reason as the engine path below: vector games have num_screens == 0, so the
			// generic restore never fires).
			if (m_config && m_chains->has_applicable_chain(0))
			{
				osd_printf_verbose("BGFX: Applying configuration (vector mode) for window %d\n", window().index());
				m_chains->load_config(*m_config->get_first_child());
				m_config.reset();
			}
		}
	}

	// ===== draw vector LINEs into the FBO =====
	// Draw directly into the FBO purely through the BGFX API, not via chain_manager / bgfx_ortho_view.
	// On success, set m_vectors_in_fbo=true; buffer_primitives skips those LINEs, and right after we
	// additively blit the FBO contents to the backbuffer.
	m_vectors_in_fbo = false;
	if (window_index == 0 && bgfx::isValid(m_vec_fb) && atlas_valid)
	{
		// Only LINEs with PRIMFLAG_VECTOR go to the FBO, to keep UI lines out of the phosphor path
		// (which would ghost them). UI / MAME-menu LINEs stay on the normal buffer_primitives path
		// (View 0, cleared each frame).
		// Vector frame statistics, published by the vector device through the render layer
		// (render_vector_stats on the primitive list) - the render-layer replacement for the
		// former direct device queries and notifiers.
		const render_vector_stats vstats = window().m_primlist->vector_stats();
		// A discontinuous MVEC seek must not combine the selected past frame with phosphor/glow
		// retained from the future frame that was visible before the seek. Recreate chain targets
		// through the normal deferred path (preserving sliders), and clear renderer-side smoothers.
		if (vstats.playback_active && vstats.playback_reset != m_vec_playback_reset)
		{
			m_vec_playback_reset = vstats.playback_reset;
			m_chains->request_temporal_reset();
			m_mglow_smoothed = 0.0f;
			m_conv_global_x = m_conv_global_y = 0.5f;
			m_conv_global_gain = 0.0f;
			m_conv_global_coverage = 0.55f;
			m_conv_global_color[0] = m_conv_global_color[1] = m_conv_global_color[2] = 1.0f;
			m_hv_smoothed = 0.0f;
			m_hv_load_norm = 0.0f;
			std::memset(m_edge_smooth, 0, sizeof(m_edge_smooth));
			m_flicker_cycle = 0;
			m_flicker_accum_ms = 0.0;
			m_flicker_prev_count = 0;
			m_flicker_prev_t0 = -1.0;
			m_flicker_prev_t1 = -1.0;
			m_vec_persist_prev_t = -1.0;
		}
		// Track whether the device drew a new emulated frame since the previous present, so pause /
		// menu stills (no emulation progress) can freeze the phosphor tail and persistence pools.
		m_vec_frame_advanced = (vstats.frame_id != m_vec_prev_frame_id);
		m_vec_prev_frame_id = vstats.frame_id;

		int vector_count = 0;   // all vector lines (decides the FBO path)
		int visible_count = 0;  // lines drawn this frame (full-frame: every vector line)
		// Point-classified count, using the SAME as_point test put_analytic_line uses (seg_len <=
		// point_threshold). Starburst rays (and only rays - see the glow buffer sizing below) are
		// drawn for dwell POINTS only, so their (large) per-primitive vertex cost must be budgeted by
		// point_count, not visible_count - reserving it for every LINE too (most of a busy scene, e.g.
		// dense BIOS/CCPU text) blew the transient vertex buffer and starved the whole glow buffer.
		int point_count = 0;
		const float pt_thresh = m_line_analytic
				? m_chains->slider_value(0, "line_point_threshold", LINE_POINT_THRESHOLD) : 0.0f;
		// Cyclic per-vector flicker (real AVG/DVG only, see render_vector_stats::timed): reproduces the
		// effect lost when the windowed beam-event draw mode was retired - real vector hardware re-traces EVERY
		// vector EVERY refresh (there is no "static image"), so a busy scene (more vectors than the
		// beam can visit before it must restart the sweep) makes a DIFFERENT rotating subset miss its
		// redraw each individual refresh, even for otherwise-unchanging content. Modelled as: divide
		// this frame's list, by GENERATION-TIME ORDER (t0, i.e. how far into the sweep each vector was
		// drawn - not list position, though the two strongly correlate for a sequential VGO), into
		// flicker_buckets equal time-slices; one bucket rotates out of the draw each PRESENT (not each
		// emulated frame - a static/paused scene still cycles through all buckets over flicker_buckets
		// presents). Only engages once the scene is busier than flicker_thresh vectors, so light scenes
		// are exactly as before (thresh 0 = always on, for testing). The excluded bucket's vectors are
		// simply not drawn this present; the existing full-frame phosphor pool decays them exactly as
		// if the CPU had not generated them - no new persistence code needed.
		// Perf: the busyness/time-span stats use the PREVIOUS present's numbers (m_flicker_prev_*),
		// updated from THIS frame's own scan below, instead of a dedicated pre-pass over the whole
		// primitive list - a chaotic, cyclic effect like this cannot perceive a one-present lag in
		// "how busy was the scene," so this trades an exact result for skipping a full O(n) traversal.
		const bool flicker_on = vstats.timed;
		const int flicker_n = flicker_on ? std::clamp(int(m_chains->slider_value(0, "flicker_buckets", 6.0f) + 0.5f), 1, 32) : 1;
		const double first_t0 = m_flicker_prev_t0, last_t1 = m_flicker_prev_t1;
		// "Busy" is judged by the REAL DRAW-TIME SPAN this present's list took to sweep (last_t1 -
		// first_t0, already tracked below for the bucket span anyway), not raw vector count: a
		// text-heavy scene (Star Wars attract-mode scores) has a huge vector COUNT from many short
		// strokes without a proportional increase in actual beam sweep time, while a scene with fewer
		// but much LONGER vectors can take just as long (or longer) to sweep with a small count - count
		// doesn't track the physical constraint (how much of a refresh period the beam spent drawing)
		// that this feature is meant to approximate, and is not comparable across games with very
		// different average line lengths. flicker_thresh_ms is in real elapsed ms (t0/t1 are seconds).
		const double flicker_thresh_ms = flicker_on ? double(m_chains->slider_value(0, "flicker_thresh_ms", 12.0f)) : 1e18;
		const double flicker_draw_ms = (last_t1 > first_t0) ? (last_t1 - first_t0) * 1000.0 : 0.0;
		const bool flicker_busy = flicker_on && flicker_draw_ms > flicker_thresh_ms;
		// Real-time-paced cycling (not once-per-PRESENT): advancing by a fixed +1 per present ties the
		// perceived flicker rate directly to whatever the ACTUAL achieved present rate happens to be -
		// a busy/heavy scene that runs below full rate (GPU-bound) cycles slower than a light one, and
		// a perf optimisation that makes busy scenes faster (e.g. the Overload Glow footprint fix, or
		// removing halation/starburst from the colour chain) silently speeds the flicker up even though
		// no flicker slider changed. Wall-clock (bx::getHPCounter, real elapsed ms - NOT emulated time,
		// so pause/slow-motion don't affect it either) with an accumulator gives a fixed cadence in Hz
		// regardless of present rate or which machine/driver is running, so flicker_period_ms means the
		// same thing on every game. Accumulator resets when inactive so a later busy stretch starts
		// clean rather than replaying a stale fractional step from long ago.
		const int64_t flicker_hpc_now = bx::getHPCounter();
		const double flicker_real_dt_ms = (m_flicker_last_hpc != 0)
				? double(flicker_hpc_now - m_flicker_last_hpc) * 1000.0 / double(bx::getHPFrequency()) : 0.0;
		// MVEC playback is a frame-domain tool: pause means zero elapsed display time, and a
		// one-frame step advances temporal effects by one recorded machine refresh.
		const double flicker_dt_ms = vstats.playback_active ? double(vstats.playback_dt_ms) : flicker_real_dt_ms;
		m_flicker_last_hpc = flicker_hpc_now;
		if (flicker_busy)
		{
			m_flicker_accum_ms += flicker_dt_ms;
			const double flicker_period_ms = std::max(1.0, double(m_chains->slider_value(0, "flicker_period_ms", 16.7f)));
			while (m_flicker_accum_ms >= flicker_period_ms)
			{
				m_flicker_accum_ms -= flicker_period_ms;
				m_flicker_cycle++;
			}
		}
		else
			m_flicker_accum_ms = 0.0;
		const double flicker_span = flicker_busy ? std::max(1e-9, (last_t1 - first_t0) / double(flicker_n)) : 1.0;
		const int flicker_active_bucket = int(m_flicker_cycle % uint64_t(flicker_n));
		// Per-channel flicker visibility (real colour monitors: green flickers most, blue barely -
		// green dominates the luminance signal flicker perception rides on, and the blue S-cone
		// pathway is temporally sluggish; emulated output on a sample-and-hold panel loses those
		// perceptual filters, so the weighting is baked in here). 1.0 = the channel drops out fully
		// in its excluded bucket (classic behaviour); smaller = the channel is only dimmed by that
		// fraction there, so it shimmers instead of blinking. All-1.0 keeps the cheap full-skip path
		// (mono/Vectrex chains have no flicker_rgb slider and stay on it).
		// Separate depth controls let blue retain a mathematically non-zero shimmer below the old
		// colour slider's 0.01 step. Legacy chains still fall back to flicker_rgb components.
		const float fl_rgb[3] = {
			flicker_busy ? std::clamp(m_chains->slider_value(0, "flicker_red_depth",   m_chains->slider_value_indexed(0, "flicker_rgb", 0, 1.0f)), 0.0f, 1.0f) : 1.0f,
			flicker_busy ? std::clamp(m_chains->slider_value(0, "flicker_green_depth", m_chains->slider_value_indexed(0, "flicker_rgb", 1, 1.0f)), 0.0f, 1.0f) : 1.0f,
			flicker_busy ? std::clamp(m_chains->slider_value(0, "flicker_blue_depth",  m_chains->slider_value_indexed(0, "flicker_rgb", 2, 1.0f)), 0.0f, 1.0f) : 1.0f };
		const bool flicker_partial = flicker_busy
				&& (fl_rgb[0] < 0.999f || fl_rgb[1] < 0.999f || fl_rgb[2] < 0.999f);
		// This frame's OWN stats, gathered for free in the scan loop below (no extra traversal),
		// cached for use as next present's first_t0/last_t1/raw_count.
		double cur_first_t0 = -1.0, cur_last_t1 = -1.0;
		int cur_raw_count = 0;
		// vector.cpp emits one full-screen black VECTORBUF quad for every vector screen. Its bounds have
		// already passed through the render core's layout, orientation and aspect-fit transforms, so they
		// are the exact, scene-independent CRT face rectangle in window pixels. Use this instead of the
		// lit-line bounding box: a sparse ranking/title screen may use the full height but only 1/3 width,
		// which used to make the beam and glow three times too thin until a later wide scene appeared.
		float screen_min_x = 0.0f, screen_min_y = 0.0f;
		float screen_max_x = float(s_width[window_index]), screen_max_y = float(s_height[window_index]);
		bool have_screen_rect = false;
		render_primitive *scan = window().m_primlist->first();
		while (scan != nullptr)
		{
			if (scan->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(scan->flags))
			{
				vector_count++;
				// Gather this frame's own busyness stats here (free - already walking every vector for
				// vector_count) for NEXT present's flicker decision; see the flicker_busy comment above.
				if (flicker_on)
				{
					cur_raw_count++;
					if (scan->t0 >= 0.0)
					{
						if (cur_first_t0 < 0.0) cur_first_t0 = scan->t0;
						cur_last_t1 = std::max(cur_last_t1, scan->t1);
					}
				}
				// This present's rotating bucket is excluded (not drawn) - the phosphor pool decays it;
				// vector_count (list membership / staleness) still counts it so a flicker-only frame is
				// not mistaken for a stale/empty one. In per-channel mode (flicker_partial) the bucket's
				// vectors are DRAWN dimmed instead of skipped, so they must be counted here - the write
				// loop's buffer sizing depends on this count staying in lockstep.
				bool flicker_excluded = false;
				if (flicker_busy && !flicker_partial && scan->t0 >= 0.0)
				{
					const int bucket = std::clamp(int((scan->t0 - first_t0) / flicker_span), 0, flicker_n - 1);
					flicker_excluded = (bucket == flicker_active_bucket);
				}
				if (!flicker_excluded)
				{
					visible_count++;
					// Squared-distance point/dot classification (no sqrt): pt_thresh is a threshold
					// compare, so comparing squares is equivalent and cheaper - same test as the write
					// loop's r_is_point below (must stay IDENTICAL, it decides ray-buffer sizing here vs
					// ray routing there).
					const float sdx = scan->bounds.x1 - scan->bounds.x0, sdy = scan->bounds.y1 - scan->bounds.y0;
					if (sdx * sdx + sdy * sdy <= pt_thresh * pt_thresh)
						point_count++;
				}
			}
			else if (scan->type == render_primitive::QUAD && PRIMFLAG_GET_VECTORBUF(scan->flags))
			{
				const float x0 = std::min(scan->bounds.x0, scan->bounds.x1);
				const float x1 = std::max(scan->bounds.x0, scan->bounds.x1);
				const float y0 = std::min(scan->bounds.y0, scan->bounds.y1);
				const float y1 = std::max(scan->bounds.y0, scan->bounds.y1);
				if (!have_screen_rect)
				{
					screen_min_x = x0; screen_max_x = x1;
					screen_min_y = y0; screen_max_y = y1;
					have_screen_rect = true;
				}
				else
				{
					// Defensive union for a view containing more than one vector screen.
					screen_min_x = std::min(screen_min_x, x0); screen_max_x = std::max(screen_max_x, x1);
					screen_min_y = std::min(screen_min_y, y0); screen_max_y = std::max(screen_max_y, y1);
				}
			}
			scan = scan->next();
		}
		if (flicker_on) { m_flicker_prev_count = cur_raw_count; m_flicker_prev_t0 = cur_first_t0; m_flicker_prev_t1 = cur_last_t1; }
		const float screen_w = screen_max_x - screen_min_x;
		m_vec_res_w = (screen_w > 1.0f)
			? std::clamp(screen_w, 64.0f, float(s_width[window_index]))
			: float(s_width[window_index]);

		// The same stable CRT-face rectangle is the physical boundary from which bezel reflection glows.
		m_edge_box_min_x = screen_min_x; m_edge_box_max_x = screen_max_x;
		m_edge_box_min_y = screen_min_y; m_edge_box_max_y = screen_max_y;

		if (vector_count > 0)
		{
			// Emulated time for this present, cached for the per-vector Energy Jitter time axis
			// (emulated so the wobble freezes on pause and tracks turbo/slow-motion).
			m_vec_time_ms = vstats.playback_active
			? vstats.playback_time_ms : window().machine().time().as_double() * 1000.0;

			// HV supply droop load: peak-track this frame's total beam energy
			// with gentle decay (so it does not flicker against vsync when a frame is stale),
			// then normalise by hv_droop_ref to a 0..1 load that put_analytic_line turns into a global
			// dim + defocus. Computed before the draw so this present's lines see the current load.
			if (m_vec_frame_advanced)
				m_hv_smoothed = std::max(vstats.total_energy, m_hv_smoothed * 0.82f);
			const float hv_ref = std::max(0.01f, m_chains->slider_value(0, "hv_droop_ref", 10.0f));
			m_hv_load_norm = std::clamp(m_hv_smoothed / hv_ref, 0.0f, 1.0f);

			// Whole-stroke energy pre-pass (renderer-side counterparts of the Vectrex driver model's
			// two list-order behaviours; only meaningful for MODEL-DERIVED energy, so both gate on
			// energy_model, and both walk the FULL list including flicker-excluded vectors - exclusion
			// is a display artifact, the beam still swept them).
			// - energy_stroke_agg: a contiguous stroke (cap_flags bit0 RAMP-on .. bit1 RAMP-off run)
			//   reads ONE aggregate speed = lit px / lit ms across the whole run, so a curve drawn as
			//   many short sub-segments gets uniform brightness instead of per-segment speed noise.
			//   Assigned only when the run has 2+ line segments (a single segment reads the same either
			//   way); dot members keep the dwell model.
			// - energy_dwell_cap: dots landing on the SAME screen spot (0.5px quantized) accumulate model
			//   energy only up to the cap - the first dot claiming a spot this frame is always full (a
			//   dazzle spot still dazzles), later dots at that spot emit what remains. Keyed by position
			//   (not list order): an earlier single-slot "current parked spot" tracker reset whenever an
			//   unrelated primitive (a different object's dot/line) fell between two same-spot text dots
			//   in draw order, which is not stable frame-to-frame (other objects reorder the list) and
			//   showed up as BIOS text randomly brightening/dimming - a position-keyed map gives the same
			//   result regardless of what else is interleaved.
			m_stroke_speed.clear();
			m_dwell_scale.clear();
			const bool stroke_agg_on = m_line_analytic && m_vs.energy_model > 0.0f && m_vs.energy_stroke_agg > 0.5f;
			const bool dwell_cap_on  = m_line_analytic && m_vs.energy_model > 0.0f && m_vs.energy_dwell_cap < 15.99f;
			if (stroke_agg_on || dwell_cap_on)
			{
				const float e_ref = (m_vec_res_w > 1.0f) ? m_vec_res_w
						: float(std::max(s_width[window_index], s_height[window_index]));
				std::vector<render_primitive*> run;   // line-segment members of the current stroke run
				double run_px = 0.0, run_ms = 0.0;
				bool in_run = false;
				auto flush_run = [&]() {
					if (run.size() >= 2 && run_px > 0.0 && run_ms > 1e-6)
					{
						const float v = float(run_px / run_ms);
						for (render_primitive *rp : run)
							m_stroke_speed.emplace(rp, v);
					}
					run.clear(); run_px = 0.0; run_ms = 0.0; in_run = false;
				};
				std::unordered_map<uint64_t, float> spot_accum;   // quantized (x,y) -> energy claimed so far
				spot_accum.reserve(64);
				for (render_primitive *p = window().m_primlist->first(); p != nullptr; p = p->next())
				{
					if (p->type != render_primitive::LINE || !PRIMFLAG_GET_VECTOR(p->flags))
						continue;
					const float ddx = p->bounds.x1 - p->bounds.x0, ddy = p->bounds.y1 - p->bounds.y0;
					const float len = sqrtf(ddx * ddx + ddy * ddy);
					const bool is_pt = (len <= pt_thresh);
					if (stroke_agg_on && p->beam_energy < 0.0f)
					{
						if (p->cap_flags & 1u)   // RAMP-on: a new stroke begins here
							{ flush_run(); in_run = true; }
						if (in_run)
						{
							if (p->t0 >= 0.0 && p->t1 > p->t0)
								run_ms += (p->t1 - p->t0) * 1000.0;
							if (!is_pt)
							{
								run_px += double(len);
								run.push_back(p);
							}
							if (p->cap_flags & 2u)   // RAMP-off: the stroke ends with this segment
								flush_run();
						}
					}
					if (dwell_cap_on && is_pt && p->beam_energy < 0.0f)
					{
						const uint64_t key = (uint64_t(uint32_t(int32_t(p->bounds.x0 * 2.0f))) << 32)
											| uint64_t(uint32_t(int32_t(p->bounds.y0 * 2.0f)));
						float &accum = spot_accum.try_emplace(key, 0.0f).first->second;
						const float predicted = generic_beam_energy(p, len, true, e_ref);
						const float remaining = std::max(0.0f, m_vs.energy_dwell_cap - accum);
						const float dscale = (predicted > 1e-6f) ? std::min(1.0f, remaining / predicted) : 1.0f;
						if (dscale < 1.0f)
							m_dwell_scale.emplace(p, dscale);
						accum += predicted * dscale;
					}
				}
				flush_run();   // a stroke still open at the list end (no RAMP-off seen)
			}

			// Vertex-dwell endpoint dots: neighbour-aware pass over the vector list in
			// draw order. A point shared by two consecutive segments is a vertex where the beam dwells in
			// proportion to how sharply it turns (straight joint -> no dwell, sharp corner / reversal ->
			// full dwell); an unshared point is a stroke terminus where the beam stops (full dwell). The
			// per-endpoint factor scales the end-cap dot in put_analytic_line. vertex_dwell 0 = off
			// (uniform caps, the old behaviour). Only meaningful for the analytic path (it draws caps).
			const float vertex_dwell = m_line_analytic ? m_chains->slider_value(0, "vertex_dwell", 0.0f) : 0.0f;
			// cap_ramp_only: when on, line end-caps appear ONLY at the source-flagged RAMP termini
			// (prim->cap_flags bit0 = stroke start / RAMP-on, bit1 = stroke end / RAMP-off), overriding the
			// geometric vertex_dwell caps. Internal joints get no cap. 0 = off (geometric/uniform caps).
			const float cap_ramp_only = m_line_analytic ? m_chains->slider_value(0, "cap_ramp_only", 0.0f) : 0.0f;
			const int cap_mode = m_line_analytic ? int(std::lround(m_vs.line_cap_mode)) : 0;
			const float junction_suppress = std::clamp(m_vs.line_cap_junction_suppress, 0.0f, 1.0f);
			const bool junction_on = cap_mode == 1 && junction_suppress > 0.0f;
			struct cap_segment
			{
				render_primitive *prim;
				float x0, y0, x1, y1, dx, dy, len2;
			};
			std::vector<cap_segment> cap_segments;
			std::unordered_map<const render_primitive*, std::pair<float, float>> vtx_boost;
			if (vertex_dwell > 0.0f || cap_mode == 1 || junction_on)
			{
				vtx_boost.reserve(size_t(vector_count) * 2);
				const render_primitive *pv = nullptr;
				float pdx = 0.0f, pdy = 0.0f;
				for (render_primitive *p = window().m_primlist->first(); p != nullptr; p = p->next())
				{
					if (p->type != render_primitive::LINE || !PRIMFLAG_GET_VECTOR(p->flags))
						continue;
					const float ddx = p->bounds.x1 - p->bounds.x0, ddy = p->bounds.y1 - p->bounds.y0;
					const float len = sqrtf(ddx * ddx + ddy * ddy);
					if (len < 1.0f)
						continue;  // dots have no direction; leave them transparent to the chain
					const float ndx = ddx / len, ndy = ddy / len;
					vtx_boost.emplace(p, std::make_pair(1.0f, 1.0f));  // default: both ends are termini
					if (junction_on)
						cap_segments.push_back({ p, p->bounds.x0, p->bounds.y0, p->bounds.x1, p->bounds.y1, ddx, ddy, len * len });
					if (pv != nullptr)
					{
						const float gx = p->bounds.x0 - pv->bounds.x1;
						const float gy = p->bounds.y0 - pv->bounds.y1;
						if (gx * gx + gy * gy < 0.25f)  // shared vertex (within 0.5 px)
						{
							const float prof = (cap_mode == 1)
								? 0.0f  // connected lit vectors: no blanking transition at this joint
								: (1.0f - (pdx * ndx + pdy * ndy)) * 0.5f;  // legacy angular dwell
							vtx_boost[pv].second = prof;  // prev segment's end
							vtx_boost[p].first   = prof;  // this segment's start
						}
					}
					pv = p; pdx = ndx; pdy = ndy;
				}
			}

			// A T junction receives the horizontal body plus the terminating line's 50% axial end and its
			// blanking-transition cap. Keep the physically useful body overlap, but attenuate only that cap
			// when an endpoint lands on the interior of a non-parallel vector. A small spatial grid avoids
			// an O(N^2) scan in text-heavy scenes.
			if (junction_on && cap_segments.size() > 1)
			{
				constexpr float CELL = 32.0f;
				const float tol = std::max(0.75f, float(s_width[window_index]) / 1920.0f);
				const float tol2 = tol * tol;
				auto cell_key = [](int x, int y) -> uint64_t
				{
					return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
				};
				std::unordered_map<uint64_t, std::vector<uint32_t>> cap_grid;
				cap_grid.reserve(cap_segments.size() * 4);
				for (uint32_t i = 0; i < cap_segments.size(); i++)
				{
					const cap_segment &seg = cap_segments[i];
					const int min_x = int(std::floor((std::min(seg.x0, seg.x1) - tol) / CELL));
					const int max_x = int(std::floor((std::max(seg.x0, seg.x1) + tol) / CELL));
					const int min_y = int(std::floor((std::min(seg.y0, seg.y1) - tol) / CELL));
					const int max_y = int(std::floor((std::max(seg.y0, seg.y1) + tol) / CELL));
					for (int cy = min_y; cy <= max_y; cy++)
						for (int cx = min_x; cx <= max_x; cx++)
							cap_grid[cell_key(cx, cy)].push_back(i);
				}
				auto suppress_at = [&](const cap_segment &self, float x, float y, float &boost)
				{
					const int cell_x = int(std::floor(x / CELL));
					const int cell_y = int(std::floor(y / CELL));
					for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++)
					{
						auto bucket = cap_grid.find(cell_key(cell_x + ox, cell_y + oy));
						if (bucket == cap_grid.end())
							continue;
						for (uint32_t index : bucket->second)
						{
							const cap_segment &other = cap_segments[index];
							if (other.prim == self.prim || other.len2 <= 1e-6f)
								continue;
							const float cross = std::abs(self.dx * other.dy - self.dy * other.dx)
								/ std::sqrt(self.len2 * other.len2);
							if (cross < 0.2f)
								continue; // collinear joins and retraces retain their normal blanking cap
							const float t = ((x - other.x0) * other.dx + (y - other.y0) * other.dy) / other.len2;
							const float end_margin = std::min(0.25f, tol / std::sqrt(other.len2));
							if (t <= end_margin || t >= 1.0f - end_margin)
								continue; // shared endpoints are handled by the existing consecutive-line logic
							const float qx = other.x0 + t * other.dx, qy = other.y0 + t * other.dy;
							const float ex = x - qx, ey = y - qy;
							if (ex * ex + ey * ey <= tol2)
							{
								boost *= 1.0f - junction_suppress;
								return;
							}
						}
					}
				};
				for (const cap_segment &seg : cap_segments)
				{
					auto boost = vtx_boost.find(seg.prim);
					if (boost == vtx_boost.end())
						continue;
					suppress_at(seg, seg.x0, seg.y0, boost->second.first);
					suppress_at(seg, seg.x1, seg.y1, boost->second.second);
				}
			}

			// Keep the displayed flare in renderer state so its attack/release can be smoothed below.
			// Candidate analysis writes a separate target; a missing target now decays instead of popping off.
			float conv_global_target_x = 0.5f, conv_global_target_y = 0.5f;
			float conv_global_target_gain = 0.0f;
			float conv_global_target_coverage = std::max(0.05f, m_vs.convergence_global_coverage);
			float conv_global_target_color[3] = { 1.0f, 1.0f, 1.0f };

			// Convergence bloom is driven by excess energy deposited on the phosphor, not by recognising
			// a particular game object or shape. Compact local maxima (projectiles, small convergences)
			// get a narrow, weak bloom of their own; only a genuinely large energetic region becomes the
			// broad boundary bloom used by events such as the Death Star explosion.
			struct convergence_bloom { float x, y, magnitude, radius, r, g, b, peak, falloff_scale; };
			static constexpr int MAX_CONVERGENCE_BLOOMS = 8;
			std::vector<convergence_bloom> conv_blooms;
			const bool conv_on = m_line_analytic && (m_vs.convergence_bloom_gain > 0.0f || m_vs.convergence_global_gain > 0.0f);
			if (conv_on)
			{
				static constexpr int CONV_W = 32, CONV_H = 32;
				std::array<float, CONV_W * CONV_H> heat = {};
				std::array<float, CONV_W * CONV_H> sum_r = {}, sum_g = {}, sum_b = {};
				std::array<uint16_t, CONV_W * CONV_H> hit_count = {};
				// Per-cell tangent equations. Their direction covariance separates a few isotropic
				// projectiles from a coherent circular sweep; their RHS gives the circle centre directly.
				std::array<float, CONV_W * CONV_H> tan_xx = {}, tan_xy = {}, tan_yy = {};
				std::array<float, CONV_W * CONV_H> tan_bx = {}, tan_by = {};
				const float sw = float(std::max<uint32_t>(1, s_width[window_index]));
				const float sh = float(std::max<uint32_t>(1, s_height[window_index]));
				const float cell_w = sw / float(CONV_W), cell_h = sh / float(CONV_H);
				const float threshold = std::max(0.0f, m_vs.convergence_bloom_threshold);
				const float ov_span = (m_vs.overload_ramp > 0.0f) ? m_vs.overload_ramp
					: std::max(1e-3f, 1.0f - m_vs.overload_threshold);
				for (render_primitive *p = window().m_primlist->first(); p != nullptr; p = p->next())
				{
					if (p->type != render_primitive::LINE || !PRIMFLAG_GET_VECTOR(p->flags)) continue;
					const float dx = p->bounds.x1 - p->bounds.x0, dy = p->bounds.y1 - p->bounds.y0;
					const float len = sqrtf(dx * dx + dy * dy);
					const bool is_point = len <= pt_thresh;
					const bool has_direction = len > 1e-4f;
					const float dir_x = has_direction ? dx / len : 0.0f;
					const float dir_y = has_direction ? dy / len : 0.0f;
					const float mid_x = 0.5f * (p->bounds.x0 + p->bounds.x1);
					const float mid_y = 0.5f * (p->bounds.y0 + p->bounds.y1);
					const float tangent_rhs = dir_x * mid_x + dir_y * mid_y;
					float stroke_speed = -1.0f;
					auto sit = m_stroke_speed.find(p);
					if (sit != m_stroke_speed.end()) stroke_speed = sit->second;
					float energy = (p->beam_energy >= 0.0f) ? p->beam_energy
						: generic_beam_energy(p, len, is_point, (m_vec_res_w > 1.0f) ? m_vec_res_w : sw, stroke_speed);
					if (p->beam_energy < 0.0f)
					{
						auto dit = m_dwell_scale.find(p);
						if (dit != m_dwell_scale.end()) energy *= dit->second;
						if (is_point) energy *= energy_object_lift(std::clamp(p->color.a, 0.0f, 1.0f), true);
					}
					float over = (energy - m_vs.overload_threshold) / ov_span;
					if (is_point) over *= std::max(1.0f, m_vs.overload_dot_gain);
					over = std::clamp(over, 0.0f, 4.0f);
					if (over <= 0.0f) continue;
					// Integrate excess energy along the actual phosphor path. The old one-hit-per-cell
					// accounting gave a very short Vectrex character stroke the same support as a line
					// crossing an entire cell. Normalising by the cell diagonal retains approximately the
					// old scale for a full-cell traversal while making short strokes proportionally weaker.
					const float cell_diagonal = sqrtf(cell_w * cell_w + cell_h * cell_h);
					const int steps = is_point ? 1 : std::max(1,
						int(std::ceil(std::max(fabsf(dx) / cell_w, fabsf(dy) / cell_h))));
					const float path_weight = is_point ? 1.0f : len / (float(steps) * cell_diagonal);
					for (int step = 0; step < steps; step++)
					{
						const float t = is_point ? 0.0f : (float(step) + 0.5f) / float(steps);
						const float x = p->bounds.x0 + dx * t, y = p->bounds.y0 + dy * t;
						const int ix = std::clamp(int(x / cell_w), 0, CONV_W - 1);
						const int iy = std::clamp(int(y / cell_h), 0, CONV_H - 1);
						const int cell = iy * CONV_W + ix;
						const float deposited = over * path_weight;
						heat[cell] += deposited;
						sum_r[cell] += p->color.r * deposited;
						sum_g[cell] += p->color.g * deposited;
						sum_b[cell] += p->color.b * deposited;
						if (hit_count[cell] != std::numeric_limits<uint16_t>::max()) hit_count[cell]++;
						if (has_direction)
						{
							tan_xx[cell] += deposited * dir_x * dir_x;
							tan_xy[cell] += deposited * dir_x * dir_y;
							tan_yy[cell] += deposited * dir_y * dir_y;
							tan_bx[cell] += deposited * dir_x * tangent_rhs;
							tan_by[cell] += deposited * dir_y * tangent_rhs;
						}
					}
				}

				// The macro field integrates a broad, individually sub-threshold ring at half the local
				// threshold. Local blooms still require the full threshold, so lowering the macro field
				// does not make ordinary text/projectiles visibly bloom by itself.
				const float macro_threshold = 0.5f * threshold;
				std::array<uint8_t, CONV_W * CONV_H> hot = {};
				std::array<uint8_t, CONV_W * CONV_H> local_hot = {};
				for (int i = 0; i < CONV_W * CONV_H; i++)
				{
					hot[i] = heat[i] > macro_threshold;
					local_hot[i] = heat[i] > threshold;
				}
				std::array<int16_t, CONV_W * CONV_H> label;
				std::array<int16_t, CONV_W * CONV_H> queue = {};
				label.fill(-1);
				int components = 0;
				for (int seed = 0; seed < CONV_W * CONV_H; seed++)
				{
					if (!hot[seed] || label[seed] >= 0) continue;
					int qread = 0, qwrite = 0;
					queue[qwrite++] = int16_t(seed); label[seed] = int16_t(components);
					while (qread < qwrite)
					{
						const int cell = queue[qread++], cx = cell % CONV_W, cy = cell / CONV_W;
						for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++)
						{
							if (!ox && !oy) continue;
							const int xx = cx + ox, yy = cy + oy;
							if (xx < 0 || xx >= CONV_W || yy < 0 || yy >= CONV_H) continue;
							const int next = yy * CONV_W + xx;
							if (hot[next] && label[next] < 0)
							{
								label[next] = int16_t(components); queue[qwrite++] = int16_t(next);
							}
						}
					}
					components++;
				}

				const float knee = std::max(0.01f, m_vs.convergence_bloom_knee);
				const float min_support = std::max(0.0f, m_vs.convergence_bloom_min_support);
				const float manual_radius = m_vs.convergence_bloom_source_radius * (m_vec_res_w / 1920.0f);
				const float cell_diagonal = sqrtf(cell_w * cell_w + cell_h * cell_h);
				std::vector<uint8_t> macro_component(size_t(components), 0);

				// Macro stage: a physically large hot area receives the broad boundary bloom. Requiring
				// spatial extent prevents several compact projectiles from becoming one giant envelope;
				// unlike the previous direction-isotropy test, this does not privilege circular/radial art.
				for (int component = 0; component < components; component++)
				{
					float centre_x = 0.0f, centre_y = 0.0f;
					float fit_xx = 0.0f, fit_xy = 0.0f, fit_yy = 0.0f, fit_bx = 0.0f, fit_by = 0.0f;
					int occupied = 0, hits = 0, hottest = -1;
					float component_signal = 0.0f;
					float component_r = 0.0f, component_g = 0.0f, component_b = 0.0f;
					for (int cell = 0; cell < CONV_W * CONV_H; cell++)
					{
						if (!hot[cell] || label[cell] != component) continue;
						centre_x += (float(cell % CONV_W) + 0.5f) * cell_w;
						centre_y += (float(cell / CONV_W) + 0.5f) * cell_h;
						occupied++; hits += hit_count[cell];
						component_signal += std::min(std::max(heat[cell] - macro_threshold, 0.0f), knee);
						component_r += sum_r[cell]; component_g += sum_g[cell]; component_b += sum_b[cell];
						fit_xx += tan_xx[cell]; fit_xy += tan_xy[cell]; fit_yy += tan_yy[cell];
						fit_bx += tan_bx[cell]; fit_by += tan_by[cell];
						if (hottest < 0 || heat[cell] > heat[hottest]) hottest = cell;
					}
					if (hottest < 0) continue;

					const float support = float(occupied) * sqrtf(float(hits));
					auto smootherstep = [](float x)
					{
						x = std::clamp(x, 0.0f, 1.0f);
						return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
					};

					// Global scatter starts below the old 24-cell cliff and reaches full spatial eligibility
					// at 32 cells. The quintic has zero slope at both ends, so individual cells entering or
					// leaving the binary connectivity field cannot visibly switch the whole face on/off.
					const float extent_response = smootherstep((float(occupied) - 16.0f) / 16.0f);
					float global_support_response = 1.0f;
					if (min_support > 0.0f)
						global_support_response = smootherstep((support - 0.60f * min_support) /
							std::max(1e-3f, 0.40f * min_support));

					// The boundary-ring macro stage keeps its existing, stricter 24-cell/85%-support gate.
					// Only the full-face scatter receives the wider continuous onset.
					float support_response = 1.0f;
					if (min_support > 0.0f)
					{
						const float support_start = 0.85f * min_support;
						const float x = std::clamp((support - support_start) /
							std::max(1e-3f, min_support - support_start), 0.0f, 1.0f);
						support_response = x * x * (3.0f - 2.0f * x);
					}

					// Fallback is the hot-cell centroid. Where the deposited paths do have a stable common
					// centre, the tangent least-squares fit keeps a partial bright arc from pulling the bloom.
					// It refines placement only; it is not a shape acceptance test.
					centre_x /= float(occupied); centre_y /= float(occupied);
					const float trace = fit_xx + fit_yy;
					const float determinant = fit_xx * fit_yy - fit_xy * fit_xy;
					const float determinant_norm = (trace > 1e-4f) ? determinant / (trace * trace) : 0.0f;
					if (determinant_norm > 0.02f)
					{
						const float fitted_x = (fit_bx * fit_yy - fit_xy * fit_by) / determinant;
						const float fitted_y = (fit_xx * fit_by - fit_xy * fit_bx) / determinant;
						if (std::isfinite(fitted_x) && std::isfinite(fitted_y)
							&& fitted_x >= -0.25f * sw && fitted_x <= 1.25f * sw
							&& fitted_y >= -0.25f * sh && fitted_y <= 1.25f * sh)
						{
							centre_x = fitted_x; centre_y = fitted_y;
						}
					}

					// A macro event also excites a very broad, low-level glass/face scatter. Drive it from
					// capped energy integrated over the whole component, rather than the hottest quadrant,
					// so an asymmetric bright arc cannot pull or overdrive the full-screen flare.
					const float global_norm = knee * std::max(6.0f, 0.25f * float(occupied));
					const float global_response = -expm1f(-component_signal / std::max(global_norm, 1e-3f));
					const float global_gain = m_vs.convergence_global_gain * global_response
						* extent_response * global_support_response;
					if (global_gain > conv_global_target_gain)
					{
						conv_global_target_x = std::clamp(centre_x / sw, 0.0f, 1.0f);
						conv_global_target_y = std::clamp(centre_y / sh, 0.0f, 1.0f);
						conv_global_target_gain = global_gain;
						conv_global_target_coverage = std::max(0.05f, m_vs.convergence_global_coverage);
						const float component_peak = std::max({component_r, component_g, component_b, 1e-4f});
						conv_global_target_color[0] = component_r / component_peak;
						conv_global_target_color[1] = component_g / component_peak;
						conv_global_target_color[2] = component_b / component_peak;
					}

					if (occupied < 24 || support_response <= 0.0f) continue;

					std::vector<float> radii;
					radii.reserve(size_t(occupied));
					for (int cell = 0; cell < CONV_W * CONV_H; cell++)
					{
						if (!hot[cell] || label[cell] != component) continue;
						const float x = (float(cell % CONV_W) + 0.5f) * cell_w;
						const float y = (float(cell / CONV_W) + 0.5f) * cell_h;
						radii.push_back(sqrtf((x-centre_x)*(x-centre_x) + (y-centre_y)*(y-centre_y)));
					}
					std::sort(radii.begin(), radii.end());
					const size_t outer_i = std::min(radii.size()-1, size_t(float(radii.size()-1)*0.875f + 0.5f));
					const size_t inner_i = std::min(radii.size()-1, size_t(float(radii.size()-1)*0.125f + 0.5f));
					const float auto_outer_radius = radii[outer_i] + 0.5f * cell_diagonal;
					const float auto_inner_radius = std::max(0.0f, radii[inner_i] - 0.5f * cell_diagonal);
					float radius = manual_radius > 0.0f ? manual_radius : auto_outer_radius;
					radius = std::min(radius, 0.45f * std::min(sw, sh));

					const bool split_annulus = manual_radius <= 0.0f
						&& auto_inner_radius > 1.5f * cell_diagonal
						&& auto_outer_radius - auto_inner_radius > 2.25f * cell_diagonal;
					const float signal = heat[hottest] - threshold;
					const float energy_response = -expm1f(-signal / knee);
					const float magnitude = m_vs.convergence_bloom_gain * energy_response * support_response;
					const float colour_peak = std::max({sum_r[hottest],sum_g[hottest],sum_b[hottest],1e-4f});
					const float bloom_r = sum_r[hottest]/colour_peak;
					const float bloom_g = sum_g[hottest]/colour_peak;
					const float bloom_b = sum_b[hottest]/colour_peak;
					macro_component[size_t(component)] = 1;
					if (magnitude > 1e-4f)
					{
						conv_blooms.push_back({centre_x,centre_y,magnitude,radius,
							bloom_r,bloom_g,bloom_b,heat[hottest],1.0f});
						if (split_annulus && conv_blooms.size() < MAX_CONVERGENCE_BLOOMS)
						{
							conv_blooms.push_back({centre_x,centre_y,0.4f*magnitude,auto_inner_radius,
								bloom_r,bloom_g,bloom_b,0.99f*heat[hottest],1.0f});
						}
					}
				}

				// Local stage: find independent excess-energy peaks instead of using a component's outer
				// bounds. Each compact projectile therefore gets a small bloom; nearby projectiles do not
				// inflate one shared radius. The smooth excess integral has no object-count cutoff.
				struct local_candidate { int cell; float peak; };
				std::vector<local_candidate> local_candidates;
				for (int cell = 0; cell < CONV_W * CONV_H; cell++)
				{
					if (!local_hot[cell] || label[cell] < 0 || macro_component[size_t(label[cell])]) continue;
					const int cx = cell % CONV_W, cy = cell / CONV_W;
					bool maximum = true;
					for (int oy = -1; oy <= 1 && maximum; oy++) for (int ox = -1; ox <= 1; ox++)
					{
						if (!ox && !oy) continue;
						const int xx = cx + ox, yy = cy + oy;
						if (xx < 0 || xx >= CONV_W || yy < 0 || yy >= CONV_H) continue;
						const int neighbour = yy * CONV_W + xx;
						if (heat[neighbour] > heat[cell]
							|| (heat[neighbour] == heat[cell] && neighbour < cell)) maximum = false;
					}
					if (maximum) local_candidates.push_back({cell,heat[cell]});
				}
				std::sort(local_candidates.begin(), local_candidates.end(),
					[](const local_candidate &a, const local_candidate &b) { return a.peak > b.peak; });
				std::array<uint8_t, CONV_W * CONV_H> local_suppressed = {};
				for (const local_candidate &candidate : local_candidates)
				{
					if (conv_blooms.size() >= MAX_CONVERGENCE_BLOOMS || local_suppressed[candidate.cell]) continue;
					const int pcx = candidate.cell % CONV_W, pcy = candidate.cell / CONV_W;
					float excess_sum = 0.0f, wx = 0.0f, wy = 0.0f;
					float colour_r = 0.0f, colour_g = 0.0f, colour_b = 0.0f;
					for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++)
					{
						const int xx = pcx + ox, yy = pcy + oy;
						if (xx < 0 || xx >= CONV_W || yy < 0 || yy >= CONV_H) continue;
						const int cell = yy * CONV_W + xx;
						const float excess = std::max(0.0f, heat[cell] - threshold);
						if (excess <= 0.0f) continue;
						const float x = (float(xx) + 0.5f) * cell_w;
						const float y = (float(yy) + 0.5f) * cell_h;
						excess_sum += excess; wx += excess * x; wy += excess * y;
						colour_r += sum_r[cell]; colour_g += sum_g[cell]; colour_b += sum_b[cell];
					}
					if (excess_sum <= 0.0f) continue;
					const float centre_x = wx / excess_sum, centre_y = wy / excess_sum;
					float radial_sum = 0.0f;
					for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++)
					{
						const int xx = pcx + ox, yy = pcy + oy;
						if (xx < 0 || xx >= CONV_W || yy < 0 || yy >= CONV_H) continue;
						const int cell = yy * CONV_W + xx;
						const float excess = std::max(0.0f, heat[cell] - threshold);
						const float x = (float(xx) + 0.5f) * cell_w;
						const float y = (float(yy) + 0.5f) * cell_h;
						radial_sum += excess * ((x-centre_x)*(x-centre_x) + (y-centre_y)*(y-centre_y));
					}
					const float auto_radius = std::clamp(sqrtf(radial_sum / excess_sum) + 0.35f * cell_diagonal,
						0.35f * cell_diagonal, 1.5f * cell_diagonal);
					const float radius = manual_radius > 0.0f ? manual_radius : auto_radius;
					const float energy_response = -expm1f(-(candidate.peak - threshold) / knee);
					const float local_response = -expm1f(-excess_sum / std::max(knee, threshold));
					const float magnitude = 0.22f * m_vs.convergence_bloom_gain * energy_response * local_response;
					if (magnitude <= 1e-4f) continue;
					const float colour_peak = std::max({colour_r,colour_g,colour_b,1e-4f});
					conv_blooms.push_back({centre_x,centre_y,magnitude,radius,
						colour_r/colour_peak,colour_g/colour_peak,colour_b/colour_peak,candidate.peak,0.35f});
					for (int oy = -2; oy <= 2; oy++) for (int ox = -2; ox <= 2; ox++)
					{
						if (ox*ox + oy*oy > 4) continue;
						const int xx = pcx + ox, yy = pcy + oy;
						if (xx >= 0 && xx < CONV_W && yy >= 0 && yy < CONV_H)
							local_suppressed[yy * CONV_W + xx] = 1;
					}
				}

				std::sort(conv_blooms.begin(), conv_blooms.end(),
					[](const convergence_bloom &a, const convergence_bloom &b) { return a.peak > b.peak; });
				if (conv_blooms.size() > MAX_CONVERGENCE_BLOOMS) conv_blooms.resize(MAX_CONVERGENCE_BLOOMS);
			}

			// Glass/face scatter has a short asymmetric temporal response. This is separate from
			// phosphor persistence: it removes one-frame eligibility pops while retaining a prompt
			// flash. Paused/static presents hold the state, and MVEC discontinuities clear it above.
			const float global_dt_ms = m_vec_frame_advanced ? std::max(0.0f, float(flicker_dt_ms)) : 0.0f;
			const float global_tau_ms = (conv_global_target_gain > m_conv_global_gain) ? 50.0f : 140.0f;
			const float global_alpha = (global_dt_ms > 0.0f)
				? 1.0f - expf(-global_dt_ms / global_tau_ms) : 0.0f;
			if (conv_global_target_gain > 1e-6f)
			{
				if (m_conv_global_gain <= 1e-6f)
				{
					m_conv_global_x = conv_global_target_x;
					m_conv_global_y = conv_global_target_y;
					m_conv_global_coverage = conv_global_target_coverage;
					for (int channel = 0; channel < 3; channel++)
						m_conv_global_color[channel] = conv_global_target_color[channel];
				}
				else
				{
					m_conv_global_x += global_alpha * (conv_global_target_x - m_conv_global_x);
					m_conv_global_y += global_alpha * (conv_global_target_y - m_conv_global_y);
					m_conv_global_coverage += global_alpha * (conv_global_target_coverage - m_conv_global_coverage);
					for (int channel = 0; channel < 3; channel++)
						m_conv_global_color[channel] += global_alpha
							* (conv_global_target_color[channel] - m_conv_global_color[channel]);
				}
			}
			m_conv_global_gain += global_alpha * (conv_global_target_gain - m_conv_global_gain);
			if (m_conv_global_gain < 1e-6f)
				m_conv_global_gain = 0.0f;

			// Deflection-amplifier dynamics: when on, each analytic line is drawn as a
			// DEFL_NOUT-quad polyline following the simulated beam trajectory, so the body grows from 6 to
			// DEFL_NOUT*6 verts (+ the two caps). The beam integrator state is reset at the start of each
			// frame's draw. Needs the analytic path; 0 = off (exact straight lines, 18 verts as before).
			m_defl_on = m_line_analytic && (m_chains->slider_value(0, "deflection_dynamics", 0.0f) > 0.0f);
			m_beam_valid = false;
			m_lin_valid = false;

			// Analytic glow (additional-ideas A-1b): draw each line a second time as a wide, low-amplitude
			// gaussian that follows the beam exactly - a physical PSF tail, no pyramid and no temporal lag.
			// Drawn into a SEPARATE FBO (m_vec_glow_fb) so a chain pass can add it AFTER the shadow mask
			// (scattered light is unmasked). 6 verts/line into that buffer; 0 = off.
			// Modern mono/Vectrex chains route explicit halation rim/fill and starburst rays into a
			// second post-mask optical FBO; legacy chains retain the original shared-glow path.
			// Pack the glow buffer: give a 6-vertex slot only to the components active this frame, so a
			// chain that uses just analytic_glow emits 6 verts/line instead of the full 24 (the other
			// three were written as degenerate quads but still cost vertex processing).
			const bool g_glow  = m_chains->slider_value(0, "analytic_glow", 0.0f) > 0.0f;
			const bool g_halation = m_chains->slider_value(0, "halation_gain", 1.0f) > 0.0f;
			const bool g_ring  = g_halation && m_chains->slider_value(0, "ring_gain", 0.0f) > 0.0f;
			const bool g_fill  = g_halation && m_chains->slider_value(0, "ring_fill", 0.0f) > 0.0f;
			const bool g_flare = m_chains->slider_value(0, "intensity_overdrive", 0.0f) > 0.0f;
			const bool g_oglow = g_flare && m_chains->slider_value(0, "overload_glow_gain", 0.0f) > 0.0f;
			const bool g_conv = !conv_blooms.empty();
			const int  g_rays  = (m_chains->slider_value(0, "ray_gain", 0.0f) > 0.0f)
					? int(std::clamp(m_chains->slider_value(0, "ray_count", 6.0f), 1.0f, 12.0f)) : 0;
			// cap_no_persist: line end caps move from the core buffer to the glow buffer - the glow path
			// is composited AFTER the phosphor pool, so the endpoint dot is bright at the drawing
			// instant but leaves NO afterimage (the pool's peak-hold otherwise kept the slightly
			// brighter/wider endpoints visible after the line body had decayed, turning a moving
			// object's trail into a dotted line of cap ghosts).
			m_caps_glow = m_chains->slider_value(0, "cap_no_persist", 0.0f) > 0.0f;
			m_optical_separate = bgfx::isValid(m_vec_optical_fb);
			m_glow_on = m_line_analytic && bgfx::isValid(m_vec_glow_fb)
				&& (g_glow || g_flare || g_oglow || g_conv || (!m_optical_separate && (g_ring || g_fill || g_rays > 0)));
			int goff = 0, ooff = 0;
			m_glow_off_glow = m_glow_off_ring = m_glow_off_fill = m_glow_off_flare = m_glow_off_oglow = -1;
			m_glow_rays_n = 0;
			if (g_glow)  { m_glow_off_glow  = goff; goff += 6; }
			if (g_flare) { m_glow_off_flare = goff; goff += 6; }
			if (g_oglow) { m_glow_off_oglow = goff; goff += 6; }
			if (g_ring)  { m_glow_off_ring  = m_optical_separate ? ooff : goff; if (m_optical_separate) ooff += 6; else goff += 6; }
			if (g_fill)  { m_glow_off_fill  = m_optical_separate ? ooff : goff; if (m_optical_separate) ooff += 6; else goff += 6; }
			m_glow_vpl = goff;
			m_optical_vpl = ooff;
			// Rays: own budget, sized by point_count (see below), not folded into m_glow_vpl.
			m_glow_rays_n = g_rays;
			m_ray_vpl = g_rays ? (6 * g_rays * GLOW_RAY_SEGS) : 0;

			// fill vertex data (classic: quad + rounded fans; analytic: one expanded quad)
			int vertices = 0;
			// analytic core: body quad 6 + two end-cap dots 6+6 (or DEFL_NOUT*6 body + 12 caps with deflection)
			const uint32_t verts_per_line = m_line_analytic
					? ((m_defl_on ? uint32_t(DEFL_NOUT * 6) : 6u) + (m_caps_glow ? 0u : 12u))
					: uint32_t(LINE_VERTICES_PER_LINE);
			m_vec_vpl = verts_per_line;
			const bgfx::VertexLayout &line_decl = m_line_analytic ? AnalyticLineVertex::ms_decl : ScreenVertex::ms_decl;
			bgfx::TransientVertexBuffer tvb = {};
			bgfx::TransientVertexBuffer glow_tvb = {};
			int glow_verts = 0;
			bool glow_alloc = false;
			bgfx::TransientVertexBuffer optical_tvb = {};
			int optical_verts = 0;
			bool optical_alloc = false;
			// No-persist buffer: fixed 18 verts/line - slots [0..5] no-persist dot, [6..11] cap0,
			// [12..17] cap1. Active only in cap_no_persist mode; drawn into m_vec_np_fb.
			const bool np_on = m_line_analytic && m_caps_glow && bgfx::isValid(m_vec_np_fb);
			static constexpr uint32_t NP_VPL = 18;
			bgfx::TransientVertexBuffer np_tvb = {};
			int np_verts = 0;
			bool np_alloc = false;
			// Starburst rays: own buffer sized by POINT_COUNT x m_ray_vpl, not visible_count - see the
			// m_ray_vpl comment in drawbgfx.h. Modern chains submit it to m_vec_optical_fb;
			// legacy chains keep submitting it to m_vec_glow_fb.
			bgfx::TransientVertexBuffer ray_tvb = {};
			int ray_verts = 0;
			bool ray_alloc = false;
			bgfx::TransientVertexBuffer conv_tvb = {};
			bool conv_alloc = false;
			const bool ray_on = m_line_analytic && m_ray_vpl > 0 && bgfx::isValid(m_optical_separate ? m_vec_optical_fb : m_vec_glow_fb);
			if (visible_count > 0)
			{
				const uint32_t needed = uint32_t(visible_count) * verts_per_line;
				if (needed == bgfx::getAvailTransientVertexBuffer(needed, line_decl))
				{
					bgfx::allocTransientVertexBuffer(&tvb, needed, line_decl);
					if (tvb.data)
					{
						// Best-effort separate glow buffer (6 verts/line). If it cannot be allocated the
						// core still draws; glow is simply skipped this frame.
						if (m_glow_on && m_glow_vpl > 0)
						{
							const uint32_t gneeded = uint32_t(visible_count) * uint32_t(m_glow_vpl);
							if (gneeded == bgfx::getAvailTransientVertexBuffer(gneeded, line_decl))
							{
								bgfx::allocTransientVertexBuffer(&glow_tvb, gneeded, line_decl);
								glow_alloc = (glow_tvb.data != nullptr);
							}
						}
						// Explicit point-optics buffer: halation rim/fill bypass tail shaping.
						if (m_line_analytic && m_optical_vpl > 0 && bgfx::isValid(m_vec_optical_fb))
						{
							const uint32_t oneeded = uint32_t(visible_count) * uint32_t(m_optical_vpl);
							if (oneeded == bgfx::getAvailTransientVertexBuffer(oneeded, line_decl))
							{
								bgfx::allocTransientVertexBuffer(&optical_tvb, oneeded, line_decl);
								optical_alloc = (optical_tvb.data != nullptr);
							}
						}
						// Best-effort separate no-persist buffer (18 verts/line). If it cannot be
						// allocated the core still draws; the caps/dots are simply skipped this frame.
						if (np_on)
						{
							const uint32_t npneeded = uint32_t(visible_count) * NP_VPL;
							if (npneeded == bgfx::getAvailTransientVertexBuffer(npneeded, line_decl))
							{
								bgfx::allocTransientVertexBuffer(&np_tvb, npneeded, line_decl);
								np_alloc = (np_tvb.data != nullptr);
							}
						}
						// Ray buffer: sized by point_count (dwell dots only), not visible_count.
						if (ray_on && point_count > 0)
						{
							const uint32_t rneeded = uint32_t(point_count) * uint32_t(m_ray_vpl);
							if (rneeded == bgfx::getAvailTransientVertexBuffer(rneeded, line_decl))
							{
								bgfx::allocTransientVertexBuffer(&ray_tvb, rneeded, line_decl);
								ray_alloc = (ray_tvb.data != nullptr);
							}
						}
						// One boundary ring per detected overload object, independent of per-line glow allocation.
						const uint32_t conv_needed = uint32_t(conv_blooms.size()) * 6u;
						if (g_conv && conv_needed == bgfx::getAvailTransientVertexBuffer(conv_needed, line_decl))
						{
							bgfx::allocTransientVertexBuffer(&conv_tvb, conv_needed, line_decl);
							conv_alloc = (conv_tvb.data != nullptr);
							if (conv_alloc)
							{
								AnalyticLineVertex *cv = reinterpret_cast<AnalyticLineVertex *>(conv_tvb.data);
								const float base_falloff = std::max(1.0f, m_vs.convergence_bloom_falloff * (m_vec_res_w / 1920.0f));
								for (size_t bloom_index = 0; bloom_index < conv_blooms.size(); bloom_index++)
								{
									const convergence_bloom &bloom = conv_blooms[bloom_index];
									const float falloff = std::max(1.0f, base_falloff * bloom.falloff_scale);
									const float pad = bloom.radius + 3.5f * falloff + 0.5f;
									const uint32_t rgba = u32Color(uint32_t(std::clamp(bloom.r,0.0f,1.0f)*255.0f+0.5f),
										uint32_t(std::clamp(bloom.g,0.0f,1.0f)*255.0f+0.5f),
										uint32_t(std::clamp(bloom.b,0.0f,1.0f)*255.0f+0.5f),
										uint32_t(std::min(bloom.magnitude,1.0f)*255.0f+0.5f));
									const float z = std::max(0.0f, bloom.magnitude - 1.0f);
									AnalyticLineVertex *bv = cv + bloom_index * 6;
									auto cvtx = [&](int n, float x, float y, float a, float d) {
										bv[n].m_x=x; bv[n].m_y=y; bv[n].m_z=z; bv[n].m_rgba=rgba;
										bv[n].m_u=0.0f; bv[n].m_v=0.0f; bv[n].m_a=a; bv[n].m_b=bloom.radius;
										bv[n].m_d=d; bv[n].m_sigma=-falloff;
									};
									cvtx(0,bloom.x-pad,bloom.y-pad,-pad,-pad); cvtx(1,bloom.x+pad,bloom.y-pad,pad,-pad);
									cvtx(2,bloom.x+pad,bloom.y+pad,pad,pad);   cvtx(3,bloom.x-pad,bloom.y-pad,-pad,-pad);
									cvtx(4,bloom.x+pad,bloom.y+pad,pad,pad);   cvtx(5,bloom.x-pad,bloom.y+pad,-pad,pad);
								}
							}
						}
						render_primitive *vprim = window().m_primlist->first();
						while (vprim != nullptr)
						{
							// Write only LINEs with PRIMFLAG_VECTOR. UI lines are drawn normally by
							// buffer_primitives (to avoid phosphor ghosting). Cyclic flicker exclusion
							// (see flicker_busy above) MUST match the visible_count scan exactly (that
							// count sized the allocation), so an excluded vector is skipped here too -
							// not drawn this present, same as if the CPU had not generated it. In
							// per-channel mode (flicker_partial) the bucket's vectors are drawn dimmed
							// by flicker_rgb instead (and were counted by the scan).
							const bool vp_in_bucket = flicker_busy && vprim->t0 >= 0.0
								&& std::clamp(int((vprim->t0 - first_t0) / flicker_span), 0, flicker_n - 1) == flicker_active_bucket;
							const bool vp_flicker_excluded = vp_in_bucket && !flicker_partial;
							if (vprim->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(vprim->flags) && !vp_flicker_excluded)
							{
								// Per-channel flicker is applied in place for the draw calls below and
								// restored afterward (the same primitive list may be walked again). A
								// channel at weight 1.0 goes fully dark (classic), at 0.15 it only dips
								// 15% - it shimmers rather than blinks.
								const bool vp_dimmed = vp_in_bucket && flicker_partial;
								render_color vp_saved_color;
								if (vp_dimmed)
								{
									vp_saved_color = vprim->color;
									vprim->color.r *= 1.0f - fl_rgb[0];
									vprim->color.g *= 1.0f - fl_rgb[1];
									vprim->color.b *= 1.0f - fl_rgb[2];
								}
								if (m_line_analytic)
								{
									float scap = 1.0f, ecap = 1.0f;
									if (cap_mode == 3)
									{
										scap = ecap = 0.0f;
									}
									else if (cap_mode == 2 || (cap_mode == 0 && cap_ramp_only > 0.5f))
									{
										// caps only at driver-flagged RAMP termini (bit0 start, bit1 end)
										scap = (vprim->cap_flags & 1u) ? 1.0f : 0.0f;
										ecap = (vprim->cap_flags & 2u) ? 1.0f : 0.0f;
									}
									else if (cap_mode == 1)
									{
										auto it = vtx_boost.find(vprim);
										if (it != vtx_boost.end())
										{
											scap = it->second.first;
											ecap = it->second.second;
										}
									}
									else if (vertex_dwell > 0.0f)
									{
										auto it = vtx_boost.find(vprim);
										if (it != vtx_boost.end())
										{
											scap = 1.0f + vertex_dwell * (it->second.first  - 1.0f);
											ecap = 1.0f + vertex_dwell * (it->second.second - 1.0f);
										}
									}
									AnalyticLineVertex *gptr = glow_alloc ? reinterpret_cast<AnalyticLineVertex*>(glow_tvb.data) + glow_verts : nullptr;
									AnalyticLineVertex *optr = optical_alloc ? reinterpret_cast<AnalyticLineVertex*>(optical_tvb.data) + optical_verts : nullptr;
									AnalyticLineVertex *npptr = np_alloc ? reinterpret_cast<AnalyticLineVertex*>(np_tvb.data) + np_verts : nullptr;
									// Rays are point-only (see the m_ray_vpl comment); classify with the SAME
									// seg_len <= point_threshold test used for the point_count pre-scan and
									// put_analytic_line's internal as_point, so the reservation matches usage.
									const float rdx = vprim->bounds.x1 - vprim->bounds.x0, rdy = vprim->bounds.y1 - vprim->bounds.y0;
									const bool r_is_point = (rdx * rdx + rdy * rdy) <= (pt_thresh * pt_thresh);
									AnalyticLineVertex *rptr = (ray_alloc && r_is_point) ? reinterpret_cast<AnalyticLineVertex*>(ray_tvb.data) + ray_verts : nullptr;
									// Whole-stroke energy pre-pass results (see above): aggregate stroke
									// speed and same-spot dwell scale, if this vector received either.
									float sps = -1.0f, dsc = 1.0f;
									if (!m_stroke_speed.empty())
									{
										auto sit = m_stroke_speed.find(vprim);
										if (sit != m_stroke_speed.end()) sps = sit->second;
									}
									if (!m_dwell_scale.empty())
									{
										auto dit = m_dwell_scale.find(vprim);
										if (dit != m_dwell_scale.end()) dsc = dit->second;
									}
									put_analytic_line(vprim, reinterpret_cast<AnalyticLineVertex*>(tvb.data) + vertices, gptr, optr, npptr, rptr, scap, ecap, sps, dsc);
									if (gptr) glow_verts += m_glow_vpl;
									if (optr) optical_verts += m_optical_vpl;
									if (npptr) np_verts += NP_VPL;
									if (rptr) ray_verts += m_ray_vpl;
								}
								else
									put_solid_line(vprim, reinterpret_cast<ScreenVertex*>(tvb.data) + vertices);
								vertices += verts_per_line;
								if (vp_dimmed)
									vprim->color = vp_saved_color;
							}
							vprim = vprim->next();
						}
					}
				}
			}

			// The FBO view runs (cleared) whenever vector primitives exist: a frame whose lines
			// were all drawn in another time window must present as a dark frame - the chain's
			// phosphor decay shows through - rather than keep stale FBO content or fall back to
			// the unfiltered GUI path.

			// allocate a view for FBO drawing
			const uint16_t fbo_view = uint16_t(s_current_view);
			s_current_view++;
			bgfx::setViewFrameBuffer(fbo_view, m_vec_fb);
			// viewport: the whole FBO (post-supersample resolution)
			bgfx::setViewRect(fbo_view, 0, 0, m_vec_fb_w, m_vec_fb_h);
			bgfx::setViewClear(fbo_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
			bgfx::setViewMode(fbo_view, bgfx::ViewMode::Sequential);

			// Target bounds are back to 1x, so primitive coords are 1x too.
			// Projection covers the 1x window range; the viewport is m_vec_fb (= 2x window).
			// Rasterizing 1x coords into a 2x viewport improves sub-pixel precision on the GPU
			// (the set_bounds primitive-rounding resolution boost is gone, but the 2x rasterizer
			// resolution + fs_vector_line's analytic AA + 25-tap halo absorb it).
			float proj[16];
			const bgfx::Caps* caps = bgfx::getCaps();
			bx::mtxOrtho(proj, 0.0f,
				float(s_width[window_index]),
				float(s_height[window_index]),
				0.0f, 0.0f, 100.0f, 0.0f, caps->homogeneousDepth);
			bgfx::setViewTransform(fbo_view, nullptr, proj);

			if (vertices > 0)
			{
				bgfx::setVertexBuffer(0, &tvb);
				// no texture needed; fs_vector_line computes the fade in-shader
				bgfx_effect* line_eff = (m_line_effect != nullptr) ? m_line_effect : m_gui_effect[BLENDMODE_ADD];
				bgfx_uniform* inv = line_eff->uniform("u_inv_view_dims");
				if (inv)
				{
					float values[2] = { -1.0f / float(s_width[window_index]), 1.0f / float(s_height[window_index]) };
					inv->set(values, sizeof(float) * 2);
					inv->upload();
				}
				// u_line_params.x = Overload Softness; .y = Edge Sharpness (super-gaussian order, >1 = sharper
				// flat-topped cross-section so wide lines stay crisp). Only the body view sharpens; the glow
				// view keeps .y = 1 so the soft halo stays soft.
				bgfx_uniform* lp = line_eff->uniform("u_line_params");
				if (lp)
				{
					float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f),
									  1.0f,   // edge sharpness fixed (the line_sharpness knob was retired)
									  1.0f, m_chains->slider_value(0, "point_roundness", 0.4f) };
					lp->set(vals, sizeof(float) * 4);
					lp->upload();
				}
				line_eff->submit(fbo_view);
			}
			else
			{
				// no lines in this window: make the clear happen so the frame presents dark
				bgfx::touch(fbo_view);
			}
			m_vectors_in_fbo = true;

			// Bezel edge glow: turn the device's per-border off-screen energy bins
			// (render_vector_stats::edge_energy) into soft gaussian streaks lying on the screen-rect
			// borders (m_edge_box), drawn additively into the glow FBO below (third submit, after the
			// per-line glow and rays) so the chain's glow combine and blurs spread them like any other
			// glow source. Only the border bins a beam actually left through light up - localized,
			// unlike the whole-screen monitor glow. Off unless the active chain provides an edge_glow
			// slider > 0 (the Vectrex chain only, by default).
			bgfx::TransientVertexBuffer edge_tvb = {};
			int edge_quads = 0;
			bool edge_alloc = false;
			const float edge_gain = m_chains->slider_value(0, "edge_glow", 0.0f);
			static_assert(sizeof(m_edge_smooth) == sizeof(render_vector_stats::edge_energy));
			if (edge_gain > 0.0f && bgfx::isValid(m_vec_glow_fb)
				&& m_edge_box_max_x > m_edge_box_min_x + 8.0f && m_edge_box_max_y > m_edge_box_min_y + 8.0f)
			{
				// Temporal smoothing (instant attack, exponential release over edge_glow_persist ms of
				// real time): the raw bins follow the frame's exact sweep pattern and flicker hard;
				// phosphor/scatter persistence and the eye smooth the real thing. 0 = off (raw bins).
				const float persist_ms = m_chains->slider_value(0, "edge_glow_persist", 120.0f);
				const float e_decay = (persist_ms > 0.5f && flicker_dt_ms > 0.0)
						? powf(0.5f, float(flicker_dt_ms) / persist_ms) : 0.0f;
				for (int es = 0; es < 4; es++)
					for (int eb = 0; eb < render_vector_stats::EDGE_GLOW_BINS; eb++)
					{
						m_edge_smooth[es][eb] = std::max(vstats.edge_energy[es][eb], m_edge_smooth[es][eb] * e_decay);
						if (m_edge_smooth[es][eb] > 1e-3f)
							edge_quads++;
					}
				if (edge_quads > 0)
				{
					const uint32_t eneeded = uint32_t(edge_quads) * 6;
					if (eneeded == bgfx::getAvailTransientVertexBuffer(eneeded, AnalyticLineVertex::ms_decl))
					{
						bgfx::allocTransientVertexBuffer(&edge_tvb, eneeded, AnalyticLineVertex::ms_decl);
						edge_alloc = (edge_tvb.data != nullptr);
					}
				}
			}
			if (edge_alloc)
			{
				const float res_scale = m_vec_res_w / 1920.0f;
				const float sig  = std::max(1.0f, m_chains->slider_value(0, "edge_glow_width", 10.0f) * res_scale);
				const float half = std::max(4.0f, 0.5f * m_chains->slider_value(0, "edge_glow_length", 80.0f) * res_scale);
				const float edge_threshold = std::max(0.0f, m_chains->slider_value(0, "edge_glow_threshold", 0.0f));
				const float edge_sensitivity = std::max(0.01f, m_chains->slider_value(0, "edge_glow_sensitivity", 1.0f));
				const float epad = 3.5f * sig + 0.5f;
				AnalyticLineVertex *ev = reinterpret_cast<AnalyticLineVertex *>(edge_tvb.data);
				int vi = 0;
				constexpr int NB = render_vector_stats::EDGE_GLOW_BINS;
				// The bins live in DEVICE screen space (0=left, 1=right, 2=top, 3=bottom of the
				// device's visarea); the streaks are drawn in WINDOW space. Rotated screens (e.g.
				// the portrait Vectrex, ROT270) map a device edge onto a different window edge, so
				// transform each bin's border point + tangent through the machine orientation. (The
				// render target's orientation() reported ROT0 here - the rotation is applied inside
				// the view/container transform - so read the machine's own flags instead.)
				const u32 e_orient = window().machine().system().flags & machine_flags::MASK_ORIENTATION;
				const float e_bw = m_edge_box_max_x - m_edge_box_min_x;
				const float e_bh = m_edge_box_max_y - m_edge_box_min_y;
				for (int es = 0; es < 4; es++)
				{
					for (int eb = 0; eb < NB; eb++)
					{
						const float energy = m_edge_smooth[es][eb];
						if (energy <= 1e-3f)
							continue;
						// device-space border point (normalized) + edge tangent for this side/bin
						const float along = (float(eb) + 0.5f) / float(NB);
						float dpx = (es == 0) ? 0.0f : (es == 1) ? 1.0f : along;
						float dpy = (es == 2) ? 0.0f : (es == 3) ? 1.0f : along;
						float dtx = (es < 2) ? 0.0f : 1.0f;
						float dty = (es < 2) ? 1.0f : 0.0f;
						// device space -> window space (same orientation the prims went through)
						if (e_orient & ORIENTATION_SWAP_XY) { std::swap(dpx, dpy); std::swap(dtx, dty); }
						if (e_orient & ORIENTATION_FLIP_X) dpx = 1.0f - dpx;
						if (e_orient & ORIENTATION_FLIP_Y) dpy = 1.0f - dpy;
						const float cx = m_edge_box_min_x + dpx * e_bw;
						const float cy = m_edge_box_min_y + dpy * e_bh;
						// a streak of edge_glow_length centred on the bin, clamped to the border range,
						// lying exactly on the border line (half the gaussian shows inside = bezel leak)
						const bool horiz = (dtx != 0.0f);   // streak runs along the window x axis
						const float amin = horiz ? m_edge_box_min_x : m_edge_box_min_y;
						const float amax = horiz ? m_edge_box_max_x : m_edge_box_max_y;
						const float c = horiz ? cx : cy;
						const float ea0 = std::max(amin, c - half), ea1 = std::min(amax, c + half);
						const float ex0 = horiz ? ea0 : cx, ey0 = horiz ? cy : ea0;
						const float ex1 = horiz ? ea1 : cx, ey1 = horiz ? cy : ea1;
						const float edx = horiz ? 1.0f : 0.0f, edy = horiz ? 0.0f : 1.0f;
						const float enx = edy, eny = -edx;
						const float eseg = ea1 - ea0;
						const float signal = std::max(0.0f, energy - edge_threshold);
						const float response = -expm1f(-signal * edge_sensitivity);
						const float amp = std::clamp(response * edge_gain, 0.0f, 1.0f);
						const uint32_t ergba = u32Color(
								uint32_t(amp * 255.0f + 0.5f), uint32_t(amp * 255.0f + 0.5f),
								uint32_t(amp * 255.0f + 0.5f), 255);
						const float ga0 = -epad, ga1 = eseg + epad;
						auto evtx = [&](int i, float x, float y, float aa, float bb, float d) {
							ev[i].m_x = x; ev[i].m_y = y; ev[i].m_z = 0.0f; ev[i].m_rgba = ergba;
							ev[i].m_u = 0.0f; ev[i].m_v = 0.0f;
							ev[i].m_a = aa; ev[i].m_b = bb; ev[i].m_d = d; ev[i].m_sigma = sig;
						};
						const float sx0 = ex0 - edx * epad, sy0 = ey0 - edy * epad;
						const float sx1 = ex1 + edx * epad, sy1 = ey1 + edy * epad;
						evtx(vi + 0, sx0 + enx * epad, sy0 + eny * epad, ga0, ga0 - eseg,  epad);
						evtx(vi + 1, sx1 + enx * epad, sy1 + eny * epad, ga1, ga1 - eseg,  epad);
						evtx(vi + 2, sx1 - enx * epad, sy1 - eny * epad, ga1, ga1 - eseg, -epad);
						evtx(vi + 3, sx0 + enx * epad, sy0 + eny * epad, ga0, ga0 - eseg,  epad);
						evtx(vi + 4, sx1 - enx * epad, sy1 - eny * epad, ga1, ga1 - eseg, -epad);
						evtx(vi + 5, sx0 - enx * epad, sy0 - eny * epad, ga0, ga0 - eseg, -epad);
						vi += 6;
					}
				}
			}

			// Analytic glow: draw the separate glow buffer into m_vec_glow_fb (cleared, additive),
			// then inject it as "glow0" so a chain pass can add it after the shadow mask. Starburst rays
			// share this SAME FBO/view via a second submit from their own buffer (ray_tvb, sized by
			// point_count - see m_ray_vpl), so no chain/JSON wiring is needed for them.
			// Gated on the buffer ALLOCATION (glow_alloc || ray_alloc), not just m_glow_on: a successful
			// alloc means we own the glow FBO this frame, so we clear it - even with no geometry this
			// frame - which stops a previous frame's glow from being re-added forever (the frozen-dot
			// ghost). If BOTH allocs failed (transient-buffer pressure on a busy frame) we skip entirely
			// and LEAVE the FBO, because clearing it to black while unable to redraw the real glow would
			// make the glow vanish until the scene lightens.
			if ((glow_alloc || (!m_optical_separate && ray_alloc) || conv_alloc || edge_alloc) && bgfx::isValid(m_vec_glow_fb))
			{
				const uint16_t glow_view = uint16_t(s_current_view);
				s_current_view++;
				bgfx::setViewFrameBuffer(glow_view, m_vec_glow_fb);
				// viewport = the (possibly reduced) glow FBO; the ortho below still maps SCREEN
				// coordinates, so the geometry lands scaled onto the smaller raster automatically.
				bgfx::setViewRect(glow_view, 0, 0, m_vec_glow_fb_w, m_vec_glow_fb_h);
				bgfx::setViewClear(glow_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
				bgfx::setViewMode(glow_view, bgfx::ViewMode::Sequential);
				float gproj[16];
				bx::mtxOrtho(gproj, 0.0f, float(s_width[window_index]), float(s_height[window_index]),
					0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
				bgfx::setViewTransform(glow_view, nullptr, gproj);
				bgfx_effect* line_eff = (m_line_effect != nullptr) ? m_line_effect : m_gui_effect[BLENDMODE_ADD];
				auto set_glow_uniforms = [&]() {
					bgfx_uniform* inv = line_eff->uniform("u_inv_view_dims");
					if (inv)
					{
						float values[2] = { -1.0f / float(s_width[window_index]), 1.0f / float(s_height[window_index]) };
						inv->set(values, sizeof(float) * 2);
						inv->upload();
					}
					bgfx_uniform* lp = line_eff->uniform("u_line_params");
					if (lp)
					{
						float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f), 0.0f,
										  1.0f, 1.0f };
						lp->set(vals, sizeof(float) * 4);
						lp->upload();
					}
				};
				bool glow_submitted = false;
				if (glow_verts > 0)
				{
					bgfx::setVertexBuffer(0, &glow_tvb);
					set_glow_uniforms();
					line_eff->submit(glow_view);
					glow_submitted = true;
				}

				if (!m_optical_separate && ray_verts > 0)
				{
					bgfx::setVertexBuffer(0, &ray_tvb);
					set_glow_uniforms();
					line_eff->submit(glow_view);
					glow_submitted = true;
				}

				if (conv_alloc)
				{
					bgfx::setVertexBuffer(0, &conv_tvb);
					set_glow_uniforms();
					line_eff->submit(glow_view);
					glow_submitted = true;
				}
				if (edge_alloc)
				{
					bgfx::setVertexBuffer(0, &edge_tvb);
					set_glow_uniforms();
					line_eff->submit(glow_view);
					glow_submitted = true;
				}
				if (!glow_submitted)
					bgfx::touch(glow_view);   // no glow/ray geometry: just clear the FBO (no stale ghost)
			}

			// Explicit optical-effects FBO: point halation rim/fill and starburst rays are
			// composited directly, after ordinary glow tail shaping. Clear every frame so turning
			// either effect off cannot leave a stale optical image behind.
			if (m_optical_separate && bgfx::isValid(m_vec_optical_fb))
			{
				const uint16_t optical_view = uint16_t(s_current_view++);
				bgfx::setViewFrameBuffer(optical_view, m_vec_optical_fb);
				bgfx::setViewRect(optical_view, 0, 0, m_vec_glow_fb_w, m_vec_glow_fb_h);
				bgfx::setViewClear(optical_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
				bgfx::setViewMode(optical_view, bgfx::ViewMode::Sequential);
				float oproj[16];
				bx::mtxOrtho(oproj, 0.0f, float(s_width[window_index]), float(s_height[window_index]),
					0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
				bgfx::setViewTransform(optical_view, nullptr, oproj);
				bgfx_effect* line_eff = (m_line_effect != nullptr) ? m_line_effect : m_gui_effect[BLENDMODE_ADD];
				auto set_optical_uniforms = [&]() {
					bgfx_uniform* inv = line_eff->uniform("u_inv_view_dims");
					if (inv)
					{
						float values[2] = { -1.0f / float(s_width[window_index]), 1.0f / float(s_height[window_index]) };
						inv->set(values, sizeof(float) * 2); inv->upload();
					}
					bgfx_uniform* lp = line_eff->uniform("u_line_params");
					if (lp)
					{
						float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f), 0.0f, 1.0f, 1.0f };
						lp->set(vals, sizeof(float) * 4); lp->upload();
					}
				};
				bool optical_submitted = false;
				if (optical_verts > 0)
				{
					bgfx::setVertexBuffer(0, &optical_tvb); set_optical_uniforms();
					line_eff->submit(optical_view); optical_submitted = true;
				}
				if (ray_verts > 0)
				{
					bgfx::setVertexBuffer(0, &ray_tvb); set_optical_uniforms();
					line_eff->submit(optical_view); optical_submitted = true;
				}
				if (!optical_submitted) bgfx::touch(optical_view);
			}

			// No-persist FBO: draw the caps / short-dwell dots into m_vec_np_fb (cleared, additive),
			// then inject it as "npglow0" so a chain pass adds it back AFTER the phosphor pool - bright
			// while drawn, no afterimage, and never fed into the narrow/wide glow cascade. Uses the same
			// analytic line effect as the body view (same u_line_params), unlike the soft
			// glow view. Only when caps are routed here and the buffer was allocated.
			// Gated on the buffer ALLOCATION (see the glow block): a successful alloc means we own the
			// no-persist FBO this frame and clear it - even with no caps / junction dots - so the chain's
			// "NoPersist Combine" never re-adds a previous frame's dots forever (the dot ghost). An alloc
			// failure (busy frame) skips and leaves the FBO rather than blacking the dots out.
			if (np_alloc && bgfx::isValid(m_vec_np_fb))
			{
				const uint16_t np_view = uint16_t(s_current_view);
				s_current_view++;
				bgfx::setViewFrameBuffer(np_view, m_vec_np_fb);
				bgfx::setViewRect(np_view, 0, 0, m_vec_fb_w, m_vec_fb_h);
				bgfx::setViewClear(np_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
				bgfx::setViewMode(np_view, bgfx::ViewMode::Sequential);
				float npproj[16];
				bx::mtxOrtho(npproj, 0.0f, float(s_width[window_index]), float(s_height[window_index]),
					0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
				bgfx::setViewTransform(np_view, nullptr, npproj);
				if (np_verts > 0)
				{
					bgfx::setVertexBuffer(0, &np_tvb);
					bgfx_effect* line_eff = (m_line_effect != nullptr) ? m_line_effect : m_gui_effect[BLENDMODE_ADD];
					bgfx_uniform* inv = line_eff->uniform("u_inv_view_dims");
					if (inv)
					{
						float values[2] = { -1.0f / float(s_width[window_index]), 1.0f / float(s_height[window_index]) };
						inv->set(values, sizeof(float) * 2);
						inv->upload();
					}
					// same params as the body view: crisp dots, not soft halo
					bgfx_uniform* lp = line_eff->uniform("u_line_params");
					if (lp)
					{
						float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f),
										  1.0f,   // edge sharpness fixed (the line_sharpness knob was retired)
										  1.0f, m_chains->slider_value(0, "point_roundness", 0.4f) };
						lp->set(vals, sizeof(float) * 4);
						lp->upload();
					}
					line_eff->submit(np_view);
				}
				else
					bgfx::touch(np_view);   // no caps / junction dots: just clear the FBO (no stale ghost)
			}
		}
	}

	// chain: inject the FBO into the chain system and render it.
	// Pass m_vec_fb's color attachment (attachment 0) to the chain as "screen0".
	// The chain does a passthrough blit and writes directly to the backbuffer.
	if (m_vectors_in_fbo && window_index == 0)
	{
		const render_vector_stats vstats = window().m_primlist->vector_stats();
		bgfx::TextureHandle vec_color = bgfx::getTexture(m_vec_fb, 0);
		if (bgfx::isValid(vec_color))
		{
			const uint16_t render_w = std::max<uint16_t>(1, uint16_t(float(s_width[window_index]) * m_vec_render_scale + 0.5f));
			const uint16_t render_h = std::max<uint16_t>(1, uint16_t(float(s_height[window_index]) * m_vec_render_scale + 0.5f));
			uint16_t content_w = render_w;
			uint16_t content_h = render_h;
			float content_area = 0.0f;
			for (render_primitive *scan = window().m_primlist->first(); scan != nullptr; scan = scan->next())
			{
				if (scan->type != render_primitive::QUAD || !PRIMFLAG_GET_VECTORBUF(scan->flags))
					continue;
				const float bounds_w = std::abs(scan->bounds.x1 - scan->bounds.x0);
				const float bounds_h = std::abs(scan->bounds.y1 - scan->bounds.y0);
				const float area = bounds_w * bounds_h;
				if (area <= content_area)
					continue;
				content_area = area;
				content_w = uint16_t(std::clamp(int(std::lround(bounds_w * m_vec_render_scale)), 1, int(render_w)));
				content_h = uint16_t(std::clamp(int(std::lround(bounds_h * m_vec_render_scale)), 1, int(render_h)));
			}
			m_chains->inject_vector_screen(vec_color,
				render_w, render_h,
				m_vec_fb_w, m_vec_fb_h,
				content_w, content_h);
			// Expose the analytic-glow FBO as "glow0" for the chain's post-mask glow composite pass.
			if (bgfx::isValid(m_vec_glow_fb))
			{
				bgfx::TextureHandle glow_color = bgfx::getTexture(m_vec_glow_fb, 0);
				if (bgfx::isValid(glow_color))
					m_chains->inject_vector_glow(glow_color, m_vec_fb_w, m_vec_fb_h);
			}
			// Expose explicit optical effects separately so the final composite can bypass tail shaping.
			if (bgfx::isValid(m_vec_optical_fb))
			{
				bgfx::TextureHandle optical_color = bgfx::getTexture(m_vec_optical_fb, 0);
				if (bgfx::isValid(optical_color))
					m_chains->inject_vector_optical(optical_color, m_vec_glow_fb_w, m_vec_glow_fb_h);
			}
			// Expose the no-persist FBO as "npglow0" for the chain's post-pool cap/dot combine pass.
			if (bgfx::isValid(m_vec_np_fb))
			{
				bgfx::TextureHandle np_color = bgfx::getTexture(m_vec_np_fb, 0);
				if (bgfx::isValid(np_color))
					m_chains->inject_vector_np(np_color, m_vec_fb_w, m_vec_fb_h);
			}
			// Restore slider values from config once, the first time, after the chain is loaded.
			// For vector games num_screens=0, so the later `if (num_screens) { load_config }` block
			// never fires; it must be called explicitly here.
			if (m_config && m_chains->has_applicable_chain(0))
			{
				osd_printf_verbose("BGFX: Applying configuration (vector mode) for window %d\n", window().index());
				m_chains->load_config(*m_config->get_first_child());
				m_config.reset();
			}
			// Guard against null chain (e.g. chain change from menu, or failed load).
			if (m_chains->has_applicable_chain(0))
			{
				// Feed the monitor glow into the chain's glow pass (no-op without an "add_mglow"
				// pass). The device publishes the shaped off-screen beam energy binned by how far
				// beyond the visible area the beam reached (render_vector_stats::offscreen_energy;
				// the old mglow_threshold energy floor is baked there); here the minimum-distance
				// cutoff (mglow_min_distance, "Monitor Glow Min Distance") drops the shallow bins -
				// a beam has to leave the screen by at least that much before the tube face lights -
				// with the boundary bin blended linearly so the slider responds smoothly. The
				// chain's coefficient then scales the sum. On a stale frame the per-frame energy
				// can dip and make the glow flicker against vsync - the monitor glow physically
				// persists, so track the peak and decay it gently.
				const float mglow_min_dist = m_chains->slider_value(0, "mglow_min_distance", 0.30f);
				const float coverage_start = m_chains->slider_value(0, "mglow_coverage_start", -1.0f);
				float mglow_energy = 0.0f;
				if (coverage_start >= 0.0f)
				{
					// The deliberate Star Wars/ESB monitor flash scans an overloaded path around the
					// complete outer face. Reduce the angle/depth peak grid to one value per direction,
					// then require broad, balanced coverage. Per-cell peaks make the result independent
					// of vector subdivision/count, so bullets and dense concentric explosions cannot win
					// by accumulating many samples in one sector.
					float quadrant_energy[4] = {};
					int covered = 0;
					for (int ma = 0; ma < render_vector_stats::MONITOR_GLOW_ANGLE_BINS; ++ma)
					{
						float angle_energy = 0.0f;
						for (int mb = 0; mb < render_vector_stats::OFFSCREEN_DEPTH_BINS; ++mb)
						{
							const float bin_top = float(mb + 1) * render_vector_stats::OFFSCREEN_DEPTH_STEP;
							const float keep = std::clamp((bin_top - mglow_min_dist) /
									render_vector_stats::OFFSCREEN_DEPTH_STEP, 0.0f, 1.0f);
							angle_energy = std::max(angle_energy, vstats.monitor_glow_coverage[ma][mb] * keep);
						}
						if (angle_energy > 0.05f)
							++covered;
						quadrant_energy[ma / (render_vector_stats::MONITOR_GLOW_ANGLE_BINS / 4)] += angle_energy;
					}
					constexpr float QUADRANT_BINS = float(render_vector_stats::MONITOR_GLOW_ANGLE_BINS / 4);
					const float balanced_energy = std::min({ quadrant_energy[0], quadrant_energy[1],
							quadrant_energy[2], quadrant_energy[3] }) / QUADRANT_BINS;
					const float coverage = float(covered) / float(render_vector_stats::MONITOR_GLOW_ANGLE_BINS);
					const float coverage_full = std::max(coverage_start + 0.01f,
							m_chains->slider_value(0, "mglow_coverage_full", 0.85f));
					const float x = std::clamp((coverage - coverage_start) /
							(coverage_full - coverage_start), 0.0f, 1.0f);
					const float coverage_gain = x * x * (3.0f - 2.0f * x);
					mglow_energy = balanced_energy * coverage_gain;
				}
				else
				{
					// Compatibility for legacy chains that do not declare coverage controls.
					for (int mb = 0; mb < render_vector_stats::OFFSCREEN_DEPTH_BINS; ++mb)
					{
						const float bin_top = float(mb + 1) * render_vector_stats::OFFSCREEN_DEPTH_STEP;
						const float keep = std::clamp((bin_top - mglow_min_dist) /
								render_vector_stats::OFFSCREEN_DEPTH_STEP, 0.0f, 1.0f);
						mglow_energy += vstats.offscreen_energy[mb] * keep;
					}
				}
				const float mglow_amount = mglow_energy * m_chains->slider_value(0, "mglow_coefficient", 0.0f);
				if (m_vec_frame_advanced)
					m_mglow_smoothed = std::max(mglow_amount, m_mglow_smoothed * 0.80f);
				const float mglow_vals[4] = { m_mglow_smoothed, 0.0f, 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "add_mglow", "u_mglow_amount", mglow_vals, 4);

				// Broad convergence flare is composited directly in the final glow pass. It is post-mask
				// scattered light, clipped only to the tube face, and never warps the vector geometry.
				const float conv_global_vals[4] = { m_conv_global_x, m_conv_global_y,
					m_conv_global_gain, m_conv_global_coverage };
				const float conv_global_color[4] = { m_conv_global_color[0], m_conv_global_color[1],
					m_conv_global_color[2], 0.0f };
				for (const char *entry : { "add_mglow", "Glow Combine" })
				{
					m_chains->inject_entry_uniform(0, entry, "u_convergence_global", conv_global_vals, 4);
					m_chains->inject_entry_uniform(0, entry, "u_convergence_global_color", conv_global_color, 4);
				}

				// Ambient is reflected room light, not beam emission. The SDR seed scales the
				// completed chain by sdr_beam_level, so pre-compensate only ambient here; beam,
				// optical glow, monitor glow and bezel reflection retain the SDR exposure.
				const bool hdr_present = s_bgfx_hdr_active || s_bgfx_edr_active;
				const float sdr_level = std::max(m_chains->slider_value(0, "sdr_beam_level", 1.0f), 0.01f);
				const float ambient_output_scale = hdr_present ? 1.0f : (1.0f / sdr_level);
				const float ambient_scale_vals[4] = { ambient_output_scale, 0.0f, 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "add_mglow",   "u_ambient_output_scale", ambient_scale_vals, 4);
				m_chains->inject_entry_uniform(0, "Glow Combine", "u_ambient_output_scale", ambient_scale_vals, 4);

				// Colour phosphor-decay pool. The "Phosphor" update pass and the "Phosphor Apply"
				// compose pass share u_phos = (dt_ms, half_ms, curve, total_ms): the pool holds
				// rgb=peak/a=age and decays via the Hill sigmoid S(age). dt = emulated time advanced
				// since the previous present (0 while paused -> frozen). No-op for chains without those
				// passes. (The retired chains' tail_accum / Flicker Persist / Scan Accumulate /
				// Bloom Apply injections lived here; they went with those chains.)
				const double persist_now = window().machine().time().as_double();
				const double persist_dt = vstats.playback_active
					? double(vstats.playback_dt_ms) / 1000.0
					: ((m_vec_persist_prev_t >= 0.0 && persist_now > m_vec_persist_prev_t)
						? (persist_now - m_vec_persist_prev_t) : 0.0);
				m_vec_persist_prev_t = vstats.playback_active ? -1.0 : persist_now;
				const float phos_vals[4] = {
					float(persist_dt * 1000.0),
					m_chains->slider_value(0, "phosphor_half_ms",  42.0f),
					m_chains->slider_value(0, "phosphor_curve",    1.2f),
					m_chains->slider_value(0, "phosphor_total_ms", 500.0f) };
				m_chains->inject_entry_uniform(0, "Phosphor",       "u_phos", phos_vals, 4);
				m_chains->inject_entry_uniform(0, "Phosphor Apply", "u_phos", phos_vals, 4);
				// Energy-dependent decay rate: high-excitation phosphor saturates (second-order /
				// bimolecular recombination), so a bright deposit decays FASTER initially than a dim
				// one - a concentrated dwell dot fades toward the line level instead of out-lasting it
				// in the afterimage. Modelled by shrinking the effective half-life / total by
				// (1 + k * stored-peak) per pixel (k = phosphor_energy_decay). k = 0 = off (uniform
				// decay, unchanged). Shared by both phosphor passes so re-excite and display agree.
				const float phos2_vals[4] = {
					m_chains->slider_value(0, "phosphor_energy_decay", 0.0f),
				// .y = hold_ms: the afterglow holds full brightness this long before the decay curve
				// starts (default ~one present closes the moving-bright-line seams; 0 = old behaviour)
				m_chains->slider_value(0, "phosphor_hold_ms", 0.0f), 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "Phosphor",       "u_phos2", phos2_vals, 4);
				m_chains->inject_entry_uniform(0, "Phosphor Apply", "u_phos2", phos2_vals, 4);
				// Per-channel (RGB) phosphor decay: each colour phosphor has its own half-life (blue
				// ZnS:Ag is shorter, green longer), so the decay rate differs per channel while the
				// excitation time (age) is shared. rgb = half-life multipliers; injected with a (1,1,1)
				// fallback so the monochrome / Vectrex chains (no such slider, single phosphor) are
				// unchanged. Only meaningful on the colour chains.
				// NB: slider_value_indexed, not slider_value - colour-slider components are registered
				// as name+component ("...decay0/1/2"); plain slider_value appends the float-slider "0"
				// suffix and silently missed them, so this injection was stuck at the (1,1,1) fallback
				// (the JSON defaults never applied and the menu slider did nothing).
				const float phos_rgb_vals[4] = {
					m_chains->slider_value_indexed(0, "phosphor_rgb_decay", 0, 1.0f),
					m_chains->slider_value_indexed(0, "phosphor_rgb_decay", 1, 1.0f),
					m_chains->slider_value_indexed(0, "phosphor_rgb_decay", 2, 1.0f), 0.0f };
				m_chains->inject_entry_uniform(0, "Phosphor",       "u_phos_rgb", phos_rgb_vals, 4);
				m_chains->inject_entry_uniform(0, "Phosphor Apply", "u_phos_rgb", phos_rgb_vals, 4);

				uint32_t chain_views = m_chains->process_screen_chains(s_current_view, window());
				s_current_view += chain_views;
			}
		}
	}


	// Force-allocate m_ortho_view (the UI view) at a late index.
	// Without this, when every primitive in the frame is skipped as VECTOR/VECTORBUF, the lazy
	// allocation inside buffer_primitives never runs, m_ortho_view stays null, and the later
	// `m_gui_effect[blend]->submit(m_ortho_view->get_index())` null-derefs.
	if (window_index == 0)
	{
		// HDR-type chain detection: the active chain declares a "screen_hdr" target (linear vector
		// output). Drives the whole HDR composite path; independent of whether the swapchain is
		// actually HDR10 (SDR fallback still composites, gamma-encoded by the present pass).
		bgfx_target *const screen_hdr = m_chains->has_applicable_chain(0)
			? m_chains->targets().target(0, "screen_hdr") : nullptr;
		m_vec_hdr_chain = (window_index == 0) && (screen_hdr != nullptr);

		if (m_vec_hdr_chain)
		{
			// (re)create the linear work target (absolute nits): vector screen + artwork composited
			const uint16_t uw = uint16_t(s_width[0]);
			const uint16_t uh = uint16_t(s_height[0]);
			if (uw > 0 && uh > 0 && (m_hdr_work == nullptr || m_hdr_work->width() != uw || m_hdr_work->height() != uh))
				m_hdr_work = m_targets->create_target("hdr_work", bgfx::TextureFormat::RG11B10F, uw, uh, 1, 1, TARGET_STYLE_CUSTOM, false, false, 1.0f, 0);
			// Safety: drop to the plain SDR path if the work target could not be created.
			if (m_hdr_work == nullptr)
				m_vec_hdr_chain = false;
		}

		if (m_vec_hdr_chain)
		{
			const float w = float(s_width[0]);
			const float h = float(s_height[0]);
			const float beam_peak = m_chains->slider_value(0, "beam_peak_nits", 1000.0f);
			const float paper_white = m_module().paper_white_nits();
			// Keep the HDR beam calibration in absolute nits, but give SDR its own normalized
			// beam level. Reusing beam_peak in SDR made a nominal beam 330/200 = 1.65 with
			// the common defaults, clipping it before useful SDR-only shaping could occur.
			// Chains without the new slider retain unity SDR seeding for compatibility.
			const bool hdr_present = s_bgfx_hdr_active || s_bgfx_edr_active;
			const float sdr_beam_level = std::clamp(m_chains->slider_value(0, "sdr_beam_level", 1.0f), 0.0f, 1.0f);
			const float seed_peak = hdr_present ? beam_peak : paper_white * sdr_beam_level;

			// Seed pass: hdr_work = screen_hdr * seed_peak (linear nits). A dedicated view before
			// the artwork view; overwrites the whole target so no clear is needed.
			const uint16_t seed_view = uint16_t(s_current_view++);
			bgfx::setViewFrameBuffer(seed_view, m_hdr_work->target());
			bgfx::setViewRect(seed_view, 0, 0, uint16_t(w), uint16_t(h));
			bgfx::setViewClear(seed_view, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
			bgfx::setViewMode(seed_view, bgfx::ViewMode::Sequential);
			float seed_proj[16];
			bx::mtxOrtho(seed_proj, 0.0f, w, h, 0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
			bgfx::setViewTransform(seed_view, nullptr, seed_proj);
			if (6 == bgfx::getAvailTransientVertexBuffer(6, ScreenVertex::ms_decl) && m_hdr_screen_effect != nullptr)
			{
				bgfx::TransientVertexBuffer vb; bgfx::allocTransientVertexBuffer(&vb, 6, ScreenVertex::ms_decl);
				ScreenVertex *v = reinterpret_cast<ScreenVertex *>(vb.data);
				vertex(&v[0], 0,0,0,0xffffffff,0,0); vertex(&v[1], w,0,0,0xffffffff,1,0); vertex(&v[2], w,h,0,0xffffffff,1,1);
				vertex(&v[3], 0,0,0,0xffffffff,0,0); vertex(&v[4], w,h,0,0xffffffff,1,1); vertex(&v[5], 0,h,0,0xffffffff,0,1);
				bgfx_uniform *p = m_hdr_screen_effect->uniform("u_hdr_params");
				if (p) { float val[4] = { seed_peak, 0,0,0 }; p->set(val, sizeof(float)*4); p->upload(); }
				bgfx_uniform *st = m_hdr_screen_effect->uniform("s_tex");
				// Bind a fallback if screen_hdr has not been written yet this frame (e.g. the chain
				// did not run because the FBO/atlas was not ready): an unbound sampler is fatal on Metal.
				const bgfx::TextureHandle seed_src = bgfx::isValid(screen_hdr->texture())
						? screen_hdr->texture() : m_chains->textures().dummy_handle();
				if (st)
					bgfx::setTexture(0, st->handle(), seed_src,
						BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP);
				bgfx::setVertexBuffer(0, &vb);
				m_hdr_screen_effect->submit(seed_view);
			}

			// EDR-only SDR-content level: under macOS EDR the UI/artwork white is anchored to 1.0 =
			// the display's SDR reference white (= the brightness setting), which can read much
			// brighter than Windows HDR10's fixed paper_white nits. paper_white only moves the vector
			// (beam_peak/paper_white) because the UI cancels to paper_white/paper_white = 1.0. This
			// knob seeds the SDR-anchored content (UI + artwork blends) below paper_white so it
			// presents at < 1.0 on EDR, dimming it relative to the HDR vectors without touching their
			// brightness. Windows/SDR keep the factor at 1.0 (no change). Multiply blend stays a unit
			// ratio.
			const float edr_ui = s_bgfx_edr_active ? m_chains->slider_value(0, "edr_sdr_level", 1.0f) : 1.0f;
			// Set the per-frame nits scale on the HDR gui effects (multiply stays a unit ratio).
			for (int b = 0; b < 4; b++)
			{
				if (m_hdr_gui_effect[b] == nullptr) continue;
				bgfx_uniform *u = m_hdr_gui_effect[b]->uniform("u_hdr_gui");
				if (u) { float val[4] = { (b == BLENDMODE_RGB_MULTIPLY) ? 1.0f : (paper_white * edr_ui), 0,0,0 }; u->set(val, sizeof(float)*4); u->upload(); }
			}
		}
		setup_ortho_view();
	}

	render_primitive *prim = window().m_primlist->first();
	std::vector<void*> sources;
	while (prim != nullptr)
	{
		uint32_t blend = PRIMFLAG_GET_BLENDMODE(prim->flags);

		// allocate_buffer does not initialize the buffer when vertices==0. This happens on frames
		// where every prim is skipped (VECTOR LINE + VECTORBUF QUAD), and the later
		// bgfx::setVertexBuffer would null-deref via a garbage handle.
		// Zero-initialize explicitly so "not allocated" can be detected as data==nullptr.
		bgfx::TransientVertexBuffer buffer = {};
		allocate_buffer(prim, blend, &buffer);

		int32_t screen = -1;
		if (PRIMFLAG_GET_SCREENTEX(prim->flags))
		{
			for (screen = 0; screen < sources.size(); screen++)
			{
				if (sources[screen] == prim)
				{
					break;
				}
			}
			if (screen == sources.size())
			{
				sources.push_back(prim);
			}
		}

		buffer_status status = buffer_primitives(atlas_valid, &prim, &buffer, screen, window_index);

		if (status != BUFFER_EMPTY && status != BUFFER_SCREEN
			&& m_ortho_view && m_ortho_view->get_index() != UINT_MAX
			&& buffer.data != nullptr)
		{
			// Guard on m_ortho_view + buffer allocation. setup_ortho_view was just force-called, but
			// this prevents a null deref in abnormal states (e.g. shutdown).
			// buffer.data == nullptr means allocate_buffer returned without doing anything at
			// vertices==0 (all prims skipped); passing it to setVertexBuffer would crash inside BGFX
			// with a garbage handle.
			// HDR: packed UI quads/lines also go through the HDR gui effects into the work target.
			bgfx_effect *gui = m_vec_hdr_chain ? m_hdr_gui_effect[blend] : m_gui_effect[blend];
			bgfx::setVertexBuffer(0, &buffer);
			bgfx::setTexture(0, gui->uniform("s_tex")->handle(), m_texture_cache->texture());

			bgfx_uniform* inv_view_dims = gui->uniform("u_inv_view_dims");
			if (inv_view_dims)
			{
				float values[2] = { -1.0f / s_width[window_index], 1.0f / s_height[window_index] };
				inv_view_dims->set(values, sizeof(float) * 2);
				inv_view_dims->upload();
			}

			gui->submit(m_ortho_view->get_index());
		}

		if (status != BUFFER_DONE && status != BUFFER_PRE_FLUSH)
		{
			prim = prim->next();
		}
	}

	window().m_primlist->release_lock();

	// HDR present (section 4.1): the work target now holds vector + artwork composited in linear
	// nits. Encode it to the backbuffer (PQ for HDR10, gamma for the SDR fallback).
	if (m_vec_hdr_chain && window_index == 0 && m_hdr_present_effect != nullptr && m_hdr_work != nullptr)
	{
		if (6 == bgfx::getAvailTransientVertexBuffer(6, ScreenVertex::ms_decl))
		{
			bgfx::TransientVertexBuffer vb;
			bgfx::allocTransientVertexBuffer(&vb, 6, ScreenVertex::ms_decl);
			ScreenVertex *v = reinterpret_cast<ScreenVertex *>(vb.data);
			const float w = float(s_width[window_index]);
			const float h = float(s_height[window_index]);
			vertex(&v[0], 0.0f, 0.0f, 0.0f, 0xffffffff, 0.0f, 0.0f);
			vertex(&v[1], w,    0.0f, 0.0f, 0xffffffff, 1.0f, 0.0f);
			vertex(&v[2], w,    h,    0.0f, 0xffffffff, 1.0f, 1.0f);
			vertex(&v[3], 0.0f, 0.0f, 0.0f, 0xffffffff, 0.0f, 0.0f);
			vertex(&v[4], w,    h,    0.0f, 0xffffffff, 1.0f, 1.0f);
			vertex(&v[5], 0.0f, h,    0.0f, 0xffffffff, 0.0f, 1.0f);

			const uint16_t present_view = uint16_t(s_current_view);
			s_current_view++;
			// window 0 renders to the default backbuffer (m_framebuffer is null there).
			// While AVI recording, present into the capture target instead: update_recording()
			// reads that target back for the encoder, and render_avi_quad() copies it to the
			// backbuffer so the window keeps showing the picture during recording.
			bgfx::FrameBufferHandle present_fb = BGFX_INVALID_HANDLE;
			const bool avi_recording = m_avi_writer != nullptr && m_avi_writer->recording() && m_avi_target != nullptr;
			if (avi_recording)
				present_fb = m_avi_target->target();
			else if (m_framebuffer != nullptr)
				present_fb = m_framebuffer->target();
			bgfx::setViewFrameBuffer(present_view, present_fb);
			bgfx::setViewRect(present_view, 0, 0, uint16_t(w), uint16_t(h));
			// opaque blit fully overwrites the backbuffer (the chain wrote screen_hdr, not it)
			bgfx::setViewClear(present_view, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
			bgfx::setViewMode(present_view, bgfx::ViewMode::Sequential);
			float present_proj[16];
			bx::mtxOrtho(present_proj, 0.0f, w, h, 0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
			bgfx::setViewTransform(present_view, nullptr, present_proj);

			bgfx_uniform *hp = m_hdr_present_effect->uniform("u_hdr_params");
			if (hp)
			{
				float vals[4] = {
					m_chains->slider_value(0, "beam_peak_nits", 1000.0f),
					m_module().paper_white_nits(),
					s_bgfx_hdr_active ? 1.0f : 0.0f,
					s_bgfx_edr_active ? 1.0f : 0.0f };
				hp->set(vals, sizeof(float) * 4);
				hp->upload();
			}
			// Phosphor gamut blend: push the Rec.709 primaries toward the P22
			// phosphor chromaticities in the Rec.2020 container. 0 = off (exact 709 -> 2020).
			bgfx_uniform *pg = m_hdr_present_effect->uniform("u_phosphor_gamut");
			if (pg)
			{
				float pgv[4] = { m_chains->slider_value(0, "phosphor_gamut", 0.0f), 0.0f, 0.0f, 0.0f };
				pg->set(pgv, sizeof(float) * 4);
				pg->upload();
			}
			// Hue-preserving highlight roll-off (knee / max as multiples of beam_peak). Caps over-bright
			// additive crossings while keeping chromaticity, so a blue line crossing stays blue instead of
			// the panel desaturating it to purple. max <= knee disables. Defaults leave a single full line
			// untouched (knee 1.0) and only roll the brighter overlaps. (.zw unused: overload whitening is
			// done per-vector in put_analytic_line, tied to beam_energy, not from total pixel nits here.)
			bgfx_uniform *ro = m_hdr_present_effect->uniform("u_hdr_rolloff");
			if (ro)
			{
				float rov[4] = {
					m_chains->slider_value(0, "hdr_rolloff_knee", 1.0f),
					m_chains->slider_value(0, "hdr_rolloff_max", 1.3f),
					m_chains->slider_value(0, "hdr_sat_protect", 0.0f), 0.0f };
				ro->set(rov, sizeof(float) * 4);
				ro->upload();
			}
			// SDR-only highlight shoulder + optional shadow reshape (paper_white-relative; see
			// fs_vector_hdr_present.sc's SDR branch). Defaults [1,1,1] = off (ceiling<=knee disables
			// the roll-off, shadow_curve=1.0 is plain 1/2.2) so this is inert until tuned by eye.
			bgfx_uniform *sr = m_hdr_present_effect->uniform("u_sdr_rolloff");
			if (sr)
			{
				float srv[4] = {
					m_chains->slider_value(0, "sdr_rolloff_knee", 1.0f),
					m_chains->slider_value(0, "sdr_rolloff_ceiling", 1.0f),
					m_chains->slider_value(0, "sdr_shadow_curve", 1.0f), 0.0f };
				sr->set(srv, sizeof(float) * 4);
				sr->upload();
			}
			bgfx_uniform *st = m_hdr_present_effect->uniform("s_tex");
			const bgfx::TextureHandle present_src = (m_hdr_work && bgfx::isValid(m_hdr_work->texture()))
					? m_hdr_work->texture() : m_chains->textures().dummy_handle();
			if (st) bgfx::setTexture(0, st->handle(), present_src);
			bgfx::setVertexBuffer(0, &vb);
			m_hdr_present_effect->submit(present_view);
		}
	}

	update_hdr_diagnostics();

	// The blit block was moved before buffer_primitives (see above); nothing to do here.
	// The UI was already submitted to m_ortho_view (the late view) inside buffer_primitives.

	// This dummy draw call is here to make sure that view 0 is cleared
	// if no other draw calls are submitted to view 0.
	//bgfx::touch(s_current_view > 0 ? s_current_view - 1 : 0);

	// Advance to next frame. Rendering thread will be kicked to
	// process submitted rendering primitives.
	if (window_index == 0)
	{
		if (m_avi_writer != nullptr && m_avi_writer->recording() && window_index == 0)
		{
			render_avi_quad();
			bgfx::touch(s_current_view);
			update_recording();
		}
	}

	if (window().index() == osd_common_t::window_list().size() - 1)
	{
		s_bgfx_frame_number = bgfx::frame();
	}


	return 0;
}

namespace {

float decode_r11g11b10_channel(uint32_t bits, int mantissa_bits)
{
	const uint32_t mantissa_mask = (1U << mantissa_bits) - 1U;
	const uint32_t mantissa = bits & mantissa_mask;
	const uint32_t exponent = (bits >> mantissa_bits) & 0x1fU;
	if (exponent == 0U)
		return std::ldexp(float(mantissa), 1 - 15 - mantissa_bits);
	if (exponent == 0x1fU)
		return mantissa ? 0.0f : std::numeric_limits<float>::infinity();
	return std::ldexp(1.0f + float(mantissa) / float(1U << mantissa_bits), int(exponent) - 15);
}

} // anonymous namespace

void renderer_bgfx::destroy_hdr_diagnostics()
{
	if (bgfx::isValid(m_hdr_diag_texture))
		bgfx::destroy(m_hdr_diag_texture);
	m_hdr_diag_texture = BGFX_INVALID_HANDLE;
	m_hdr_diag_pixels.clear();
	m_hdr_diag_w = m_hdr_diag_h = 0;
	m_hdr_diag_ready_frame = 0;
	m_hdr_diag_sample_counter = 0;
	m_hdr_diag_pending = false;
}

void renderer_bgfx::process_hdr_diagnostics()
{
	if (!m_hdr_diag_pending || s_bgfx_frame_number < m_hdr_diag_ready_frame)
		return;
	m_hdr_diag_pending = false;

	const float beam_peak = m_chains->slider_value(0, "beam_peak_nits", 1000.0f);
	const float rolloff_knee = m_chains->slider_value(0, "hdr_rolloff_knee", 1.0f) * beam_peak;
	const float rolloff_ceil = m_chains->slider_value(0, "hdr_rolloff_max", 1.3f) * beam_peak;
	const float sat_protect = m_chains->slider_value(0, "hdr_sat_protect", 0.0f);
	float display_peak = m_module().hdr_display_peak_nits();
	if (display_peak <= 0.0f)
		display_peak = rolloff_ceil;
	const float hot_threshold = 0.8f * display_peak;

	float pre_peak = 0.0f;
	float post_peak = 0.0f;
	double post_sum = 0.0;
	uint64_t hot_pixels = 0;
	uint64_t over_1000 = 0;
	std::priority_queue<float, std::vector<float>, std::greater<float>> top;
	constexpr size_t TOP_COUNT = 128;

	for (uint32_t packed : m_hdr_diag_pixels)
	{
		float rgb[3] = {
			decode_r11g11b10_channel(packed, 6),
			decode_r11g11b10_channel(packed >> 11, 6),
			decode_r11g11b10_channel(packed >> 22, 5) };
		if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]))
			continue;
		const float original = std::max({ rgb[0], rgb[1], rgb[2] });
		pre_peak = std::max(pre_peak, original);
		if (rolloff_ceil > rolloff_knee && original > rolloff_knee)
		{
			const float minimum = std::min({ rgb[0], rgb[1], rgb[2] });
			const float saturation = (1.0f - minimum / std::max(original, 1e-4f)) * sat_protect;
			const float effective_ceil = rolloff_ceil + (rolloff_knee - rolloff_ceil)
				* std::clamp(saturation, 0.0f, 1.0f);
			const float over = original - rolloff_knee;
			const float range = std::max(effective_ceil - rolloff_knee, 1e-4f);
			const float rolled = rolloff_knee + range * over / (over + range);
			const float scale = rolled / original;
			rgb[0] *= scale; rgb[1] *= scale; rgb[2] *= scale;
		}
		const float post = std::max({ rgb[0], rgb[1], rgb[2] });
		post_peak = std::max(post_peak, post);
		post_sum += post;
		if (post >= hot_threshold) ++hot_pixels;
		if (post >= 1000.0f) ++over_1000;
		if (top.size() < TOP_COUNT)
			top.push(post);
		else if (post > top.top())
		{
			top.pop();
			top.push(post);
		}
	}

	double top_sum = 0.0;
	const size_t top_count = top.size();
	while (!top.empty()) { top_sum += top.top(); top.pop(); }
	const double pixel_count = double(std::max<size_t>(1, m_hdr_diag_pixels.size()));
	const float usage = display_peak > 0.0f ? 100.0f * post_peak / display_peak : 0.0f;
	const float headroom = m_module().paper_white_nits() > 0.0f
		? display_peak / m_module().paper_white_nits() : 0.0f;
	const char *mode = s_bgfx_edr_active ? "macOS EDR" : (s_bgfx_hdr_active ? "HDR10" : "SDR fallback");
	window().machine().ui().set_hdr_diagnostic_text(util::string_format(
		"HDR diagnostic (%s)\nDisplay peak: %.0f nits  Headroom: %.2fx\n"
		"Beam peak: %.0f nits  Pre-rolloff max: %.0f nits\n"
		"Post-rolloff max: %.0f nits (%.1f%%)  Top %u avg: %.0f nits\n"
		"Pixels >=80%% peak: %llu  >=1000 nits: %llu  Frame avg: %.2f nits",
		mode, display_peak, headroom, beam_peak, pre_peak, post_peak, usage,
		unsigned(top_count), top_count ? float(top_sum / double(top_count)) : 0.0f,
		(unsigned long long)hot_pixels, (unsigned long long)over_1000, float(post_sum / pixel_count)));
}

void renderer_bgfx::update_hdr_diagnostics()
{
	if (window().index() != 0)
		return;
	const bool requested = m_vec_hdr_chain && m_chains && m_chains->has_applicable_chain(0)
		&& m_chains->slider_value(0, "hdr_diagnostics", 0.0f) > 0.5f;
	if (!requested)
	{
		window().machine().ui().set_hdr_diagnostic_text(std::string());
		if (!m_hdr_diag_pending || s_bgfx_frame_number >= m_hdr_diag_ready_frame)
			destroy_hdr_diagnostics();
		return;
	}

	if (!(s_bgfx_hdr_active || s_bgfx_edr_active))
	{
		if (m_hdr_diag_pending && s_bgfx_frame_number >= m_hdr_diag_ready_frame)
			process_hdr_diagnostics();
		if (!m_hdr_diag_pending)
			destroy_hdr_diagnostics();
		window().machine().ui().set_hdr_diagnostic_text("HDR diagnostic unavailable: HDR/EDR output is not active");
		return;
	}

	process_hdr_diagnostics();
	if (m_hdr_diag_pending || ++m_hdr_diag_sample_counter < 30U || m_hdr_work == nullptr)
		return;
	m_hdr_diag_sample_counter = 0;

	const uint16_t width = m_hdr_work->width();
	const uint16_t height = m_hdr_work->height();
	if (!bgfx::isValid(m_hdr_diag_texture) || width != m_hdr_diag_w || height != m_hdr_diag_h)
	{
		destroy_hdr_diagnostics();
		m_hdr_diag_w = width;
		m_hdr_diag_h = height;
		const uint64_t flags = BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK
			| BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
			| BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
		const bgfx::Caps *caps = bgfx::getCaps();
		m_hdr_diag_supported = (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK)
			&& (caps->supported & BGFX_CAPS_TEXTURE_BLIT)
			&& bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RG11B10F, flags);
		if (!m_hdr_diag_supported)
		{
			window().machine().ui().set_hdr_diagnostic_text("HDR diagnostic unavailable: RG11B10F readback is not supported by this backend");
			return;
		}
		m_hdr_diag_texture = bgfx::createTexture2D(width, height, false, 1,
			bgfx::TextureFormat::RG11B10F, flags);
		m_hdr_diag_pixels.resize(size_t(width) * size_t(height));
		window().machine().ui().set_hdr_diagnostic_text("HDR diagnostic: sampling final linear-nits buffer...");
	}

	if (!bgfx::isValid(m_hdr_diag_texture) || m_hdr_diag_pixels.empty())
		return;
	const uint16_t diag_view = uint16_t(s_current_view++);
	bgfx::setViewName(diag_view, "HDR diagnostic readback");
	bgfx::blit(diag_view, m_hdr_diag_texture, 0, 0, m_hdr_work->texture());
	m_hdr_diag_ready_frame = bgfx::readTexture(m_hdr_diag_texture, m_hdr_diag_pixels.data());
	m_hdr_diag_pending = true;
}

void renderer_bgfx::update_recording()
{
	// s_current_view - 1 is render_avi_quad()'s view. Keying the blit to a view that sorts
	// after the HDR present view matters: bgfx runs blits when their view begins, so an
	// earlier view would read the avi target before the present pass has written this frame.
	bgfx::blit(s_current_view > 0 ? s_current_view - 1 : 0, m_avi_texture, 0, 0, bgfx::getTexture(m_avi_target->target()));
	bgfx::readTexture(m_avi_texture, m_avi_data);

	int i = 0;
	for (int y = 0; y < m_avi_bitmap.height(); y++)
	{
		uint32_t *dst = &m_avi_bitmap.pix(y);

		for (int x = 0; x < m_avi_bitmap.width(); x++)
		{
			*dst++ = 0xff000000 | (m_avi_data[i + 0] << 16) | (m_avi_data[i + 1] << 8) | m_avi_data[i + 2];
			i += 4;
		}
	}

	m_avi_writer->video_frame(m_avi_bitmap);
}

void renderer_bgfx::add_audio_to_recording(const int16_t *buffer, int samples_this_frame)
{
	if (m_avi_writer != nullptr && m_avi_writer->recording() && window().index() == 0)
	{
		m_avi_writer->audio_frame(buffer, samples_this_frame);
	}
}

bool renderer_bgfx::update_dimensions()
{
	const uint32_t window_index = window().index();
	const uint32_t width = s_width[window_index];
	const uint32_t height = s_height[window_index];

	if (m_dimensions != osd_dim(width, height))
	{
		bgfx::reset(width, height,
			(video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) | (s_bgfx_hdr_active ? BGFX_RESET_HDR10 : 0),
			s_bgfx_hdr_active ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::Count);
		m_dimensions = osd_dim(width, height);

		if (window().index() != 0)
		{
#ifdef OSD_WINDOWS
			m_framebuffer = m_targets->create_backbuffer(dynamic_cast<win_window_info &>(window()).platform_window(), width, height);
#elif defined(OSD_MAC)
			m_framebuffer = m_targets->create_backbuffer(GetOSWindow(dynamic_cast<mac_window_info &>(window()).platform_window()), width, height);
#else
			m_framebuffer = m_targets->create_backbuffer(sdlNativeWindowHandle(dynamic_cast<sdl_window_info &>(window()).platform_window()).first, width, height);
#endif
			if (m_ortho_view)
			{
				m_ortho_view->set_backbuffer(m_framebuffer);
			}
			bgfx::setViewFrameBuffer(s_current_view, m_framebuffer->target());
		}

		bgfx::setViewClear(s_current_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
		bgfx::setViewMode(s_current_view, bgfx::ViewMode::Sequential);
		bgfx::touch(s_current_view);
		bgfx::frame();
		return true;
	}
	return false;
}

void renderer_bgfx::setup_ortho_view()
{
	// Target selection:
	//  - SDR / non-HDR chain: the default backbuffer (m_framebuffer), stock behaviour
	//  - HDR-type chain: split by phase - backdrop (before the screen) into the under offscreen,
	//    bezel/menu/OSD (after) into the over offscreen; the composite pass merges them later
	if (!m_ortho_view)
	{
		m_ortho_view = std::make_unique<bgfx_ortho_view>(this, 0, m_framebuffer, m_seen_views);
		// Since the UI view is placed at a late index after the blit, m_ortho_view itself does not
		// clear the backbuffer (the manual clear view already did).
		m_ortho_view->disable_color_clear();
	}

	if (m_vec_hdr_chain && m_hdr_work != nullptr)
	{
		// Artwork/UI draws into the single linear work target (seeded with the vector screen by
		// the screen pass). Per-frame view index; no clear (the seed pass already filled it).
		if (m_hdr_work_view == UINT_MAX)
		{
			m_hdr_work_view = s_current_view++;
			const uint16_t vw = uint16_t(s_width[window().index()]);
			const uint16_t vh = uint16_t(s_height[window().index()]);
			bgfx::setViewFrameBuffer(uint16_t(m_hdr_work_view), m_hdr_work->target());
			bgfx::setViewRect(uint16_t(m_hdr_work_view), 0, 0, vw, vh);
			bgfx::setViewClear(uint16_t(m_hdr_work_view), BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
			bgfx::setViewMode(uint16_t(m_hdr_work_view), bgfx::ViewMode::Sequential);
			float proj[16];
			bx::mtxOrtho(proj, 0.0f, float(vw), float(vh), 0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
			bgfx::setViewTransform(uint16_t(m_hdr_work_view), nullptr, proj);
		}
		m_ortho_view->set_backbuffer(m_hdr_work);
		m_ortho_view->set_index(m_hdr_work_view);
		m_ortho_view->update();
		return;
	}

	// SDR / non-HDR chain: single late view on the default backbuffer (stock behaviour).
	m_ortho_view->set_backbuffer(m_framebuffer);
	if (m_ortho_view->get_index() == UINT_MAX)
	{
		m_ortho_view->set_index(s_current_view);
		m_ortho_view->setup();
		s_current_view++;
	}
	m_ortho_view->update();
}

render_primitive_list *renderer_bgfx::get_primitives()
{
	// determines whether the screen container is transformed by the chain's shaders
	bool chain_transform = false;

	// check the first chain
	bgfx_chain* chain = this->m_chains->screen_chain(0);
	if (chain != nullptr)
	{
		chain_transform = chain->transform();
	}

	osd_dim wdim = window().get_size_pixels();
	if (wdim.width() > 0 && wdim.height() > 0)
	{
		// Keep target bounds at the 1x window (2x supersample bounds cause atlas bleeding in MAME UI
		// glyph rendering = the neighboring cell's "|" appears between characters). Vector-line
		// supersampling is provided solely by the vec_fb (= 2x window) rasterizer resolution.
		window().target()->set_bounds(
			wdim.width(),
			wdim.height(),
			window().pixel_aspect());
	}

	window().target()->set_transform_container(!chain_transform);
	return &window().target()->get_primitives();
}

renderer_bgfx::buffer_status renderer_bgfx::buffer_primitives(bool atlas_valid, render_primitive** prim, bgfx::TransientVertexBuffer* buffer, int32_t screen, int window_index)
{
	int vertices = 0;

	uint32_t blend = PRIMFLAG_GET_BLENDMODE((*prim)->flags);
	while (*prim != nullptr)
	{
		switch ((*prim)->type)
		{
			case render_primitive::LINE:
				// Only LINEs with PRIMFLAG_VECTOR were drawn via the FBO; the rest (UI / MAME menu)
				// are drawn normally, directly to the View 0 backbuffer. Symmetric with the FBO-side
				// selection, so phosphor ghosting does not affect the UI.
				if (m_vectors_in_fbo && PRIMFLAG_GET_VECTOR((*prim)->flags))
					break;
				setup_ortho_view();
				// allocate_buffer leaves data==nullptr when the transient vertex pool is exhausted
				// (e.g. a large beam-window vector draw consumed it). Skip the write then - the caller
				// already refuses to submit a null buffer; writing would null-deref. vertices still
				// advances so the batch/flush flow is unchanged.
				if (buffer->data)
					put_packed_line(*prim, (ScreenVertex*)buffer->data + vertices);
				vertices += 30;
				break;

			case render_primitive::QUAD:
				// Skip the VECTORBUF background quad (the black background drawn by vector.cpp):
				// it would overwrite the vec blit, which has already filled the backbuffer.
				if (m_vectors_in_fbo && PRIMFLAG_GET_VECTORBUF((*prim)->flags))
					break;
				if ((*prim)->texture.base == nullptr)
				{
					setup_ortho_view();
					if (buffer->data)   // see put_packed_line above: null when the transient pool was exhausted
						put_packed_quad(*prim, WHITE_HASH, (ScreenVertex*)buffer->data + vertices);
					vertices += 6;
				}
				else
				{
					const uint32_t hash = get_texture_hash(*prim);
					if (atlas_valid && (*prim)->packable(PACKABLE_SIZE) && hash != 0 && m_hash_to_entry[hash].hash())
					{
						setup_ortho_view();
						if (buffer->data)   // see put_packed_line above: null when the transient pool was exhausted
							put_packed_quad(*prim, hash, (ScreenVertex*)buffer->data + vertices);
						vertices += 6;
					}
					else
					{
						if (vertices > 0)
						{
							return BUFFER_PRE_FLUSH;
						}

						if (PRIMFLAG_GET_SCREENTEX((*prim)->flags) && m_chains->has_applicable_chain(screen))
						{
#if SCENE_VIEW
							setup_view(s_current_view, true);
							render_post_screen_quad(s_current_view, *prim, buffer, screen, window_index);
							s_current_view++;
#else
							setup_ortho_view();
							render_post_screen_quad(m_ortho_view->get_index(), *prim, buffer, screen, window_index);
#endif
							return BUFFER_SCREEN;
						}
						else
						{
							setup_ortho_view();
							render_textured_quad(*prim, buffer, window_index);
							return BUFFER_EMPTY;
						}
					}
				}
				break;

			default:
				// Unhandled
				break;
		}

		if ((*prim)->next() != nullptr && PRIMFLAG_GET_BLENDMODE((*prim)->next()->flags) != blend)
		{
			break;
		}

		*prim = (*prim)->next();
	}

	if (*prim == nullptr)
	{
		return BUFFER_DONE;
	}
	if (vertices == 0)
	{
		return BUFFER_EMPTY;
	}
	return BUFFER_FLUSH;
}

void renderer_bgfx::set_bgfx_state(uint32_t blend)
{
	uint64_t flags = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS;
	bgfx::setState(flags | bgfx_util::get_blend_state(blend));
}

bool renderer_bgfx::update_atlas()
{
	bool atlas_dirty = check_for_dirty_atlas();

	if (atlas_dirty)
	{
		m_hash_to_entry.clear();

		std::vector<std::vector<rectangle_packer::packed_rectangle>> packed;
		if (m_packer.pack(m_texinfo, packed, CACHE_SIZE))
		{
			process_atlas_packs(packed);
		}
		else
		{
			packed.clear();

			m_texinfo.clear();
			m_texinfo.push_back(rectangle_packer::packable_rectangle(WHITE_HASH, PRIMFLAG_TEXFORMAT(TEXFORMAT_ARGB32), 16, 16, 16, nullptr, m_white));

			m_packer.pack(m_texinfo, packed, CACHE_SIZE);
			process_atlas_packs(packed);

			return false;
		}
	}
	return true;
}

void renderer_bgfx::process_atlas_packs(std::vector<std::vector<rectangle_packer::packed_rectangle>>& packed)
{
	for (std::vector<rectangle_packer::packed_rectangle> pack : packed)
	{
		for (rectangle_packer::packed_rectangle rect : pack)
		{
			if (rect.hash() == 0xffffffff)
			{
				continue;
			}
			m_hash_to_entry[rect.hash()] = rect;
			bgfx::TextureFormat::Enum dst_format = bgfx::TextureFormat::BGRA8;
			uint16_t pitch = rect.width();
			int width_div_factor = 1;
			int width_mul_factor = 1;
			const bgfx::Memory* mem = bgfx_util::mame_texture_data_to_bgfx_texture_data(dst_format, rect.format(), rect.rowpixels(), 0, rect.height(), rect.palette(), rect.base(), pitch, width_div_factor, width_mul_factor);
			bgfx::updateTexture2D(m_texture_cache->texture(), 0, 0, rect.x(), rect.y(), (rect.width() * width_mul_factor) / width_div_factor, rect.height(), mem, pitch);
		}
	}
}

uint32_t renderer_bgfx::get_texture_hash(render_primitive *prim)
{
#if GIBBERISH
	uint32_t xor_value = 0x87;
	uint32_t hash = 0xdabeefed;

	int bpp = 2;
	uint32_t format = PRIMFLAG_GET_TEXFORMAT(prim->flags);
	if (format == TEXFORMAT_ARGB32 || format == TEXFORMAT_RGB32)
	{
		bpp = 4;
	}

	for (int y = 0; y < prim->texture.height; y++)
	{
		uint8_t *base = reinterpret_cast<uint8_t*>(prim->texture.base) + prim->texture.rowpixels * y;
		for (int x = 0; x < prim->texture.width * bpp; x++)
		{
			hash += base[x] ^ xor_value;
		}
	}
	return hash;
#else
	//return (reinterpret_cast<size_t>(prim->texture.base)) & 0xffffffff;
	return (reinterpret_cast<size_t>(prim->texture.base) ^ reinterpret_cast<size_t>(prim->texture.palette)) & 0xffffffff;
#endif
}

bool renderer_bgfx::check_for_dirty_atlas()
{
	bool atlas_dirty = false;

	std::map<uint32_t, rectangle_packer::packable_rectangle> acquired_infos;
	for (render_primitive &prim : *window().m_primlist)
	{
		bool pack = prim.packable(PACKABLE_SIZE);
		if (prim.type == render_primitive::QUAD && prim.texture.base != nullptr && pack)
		{
			const uint32_t hash = get_texture_hash(&prim);
			// If this texture is packable and not currently in the atlas, prepare the texture for putting in the atlas
			if ((hash != 0 && m_hash_to_entry[hash].hash() == 0 && acquired_infos[hash].hash() == 0)
				|| (hash != 0 && m_hash_to_entry[hash].hash() != hash && acquired_infos[hash].hash() == 0))
			{   // Create create the texture and mark the atlas dirty
				atlas_dirty = true;

				m_texinfo.push_back(rectangle_packer::packable_rectangle(hash, prim.flags & PRIMFLAG_TEXFORMAT_MASK,
					prim.texture.width, prim.texture.height, prim.texture.rowpixels, prim.texture.palette, prim.texture.base));
				acquired_infos[hash] = m_texinfo[m_texinfo.size() - 1];
			}
		}
	}

	if (m_texinfo.size() == 1)
	{
		atlas_dirty = true;
	}

	return atlas_dirty;
}

void renderer_bgfx::allocate_buffer(render_primitive *prim, uint32_t blend, bgfx::TransientVertexBuffer *buffer)
{
	int vertices = 0;
	bool mode_switched = false;
	while (prim != nullptr && !mode_switched)
	{
		switch (prim->type)
		{
			case render_primitive::LINE:
				// Symmetric with buffer_primitives: skip only PRIMFLAG_VECTOR LINEs (drawn via the
				// FBO); reserve vertices for UI lines.
				if (m_vectors_in_fbo && PRIMFLAG_GET_VECTOR(prim->flags))
					break;
				vertices += 30;
				break;

			case render_primitive::QUAD:
				// Symmetric with the skip in buffer_primitives
				if (m_vectors_in_fbo && PRIMFLAG_GET_VECTORBUF(prim->flags))
					break;
				if (!prim->packable(PACKABLE_SIZE))
				{
					if (prim->texture.base == nullptr)
					{
						vertices += 6;
					}
					else
					{
						if (vertices == 0)
						{
							vertices += 6;
						}
						mode_switched = true;
					}
				}
				else
				{
					vertices += 6;
				}
				break;
			default:
				// Do nothing
				break;
		}

		prim = prim->next();

		if (prim != nullptr && PRIMFLAG_GET_BLENDMODE(prim->flags) != blend)
		{
			mode_switched = true;
		}
	}

	if (vertices > 0 && vertices==bgfx::getAvailTransientVertexBuffer(vertices, ScreenVertex::ms_decl))
	{
		bgfx::allocTransientVertexBuffer(buffer, vertices, ScreenVertex::ms_decl);
	}
}

std::vector<ui::menu_item> renderer_bgfx::get_slider_list()
{
	m_sliders_dirty = false;
	// All sliders now come from the chain, so nothing to append.
	return m_chains->get_slider_list();
}

void renderer_bgfx::set_sliders_dirty()
{
	m_sliders_dirty = true;
}

uint32_t renderer_bgfx::get_window_width(uint32_t index) const
{
	return s_width[index];
}

uint32_t renderer_bgfx::get_window_height(uint32_t index) const
{
	return s_height[index];
}


void renderer_bgfx::load_config(util::xml::data_node const &parentnode)
{
	util::xml::data_node const *windownode = parentnode.get_child("window");
	while (windownode)
	{
		if (windownode->get_attribute_int("index", -1) != window().index())
		{
			windownode = windownode->get_next_sibling("window");
			continue;
		}

		if (!m_config)
			m_config = util::xml::file::create();
		else
			m_config->get_first_child()->delete_node();
		windownode->copy_into(*m_config);
		m_config->get_first_child()->set_attribute("persist", "0");
		osd_printf_verbose("BGFX: Found configuration for window %d\n", window().index());
		break;
	}
}


void renderer_bgfx::save_config(util::xml::data_node &parentnode)
{
	if (m_config)
		m_config->get_first_child()->copy_into(parentnode);
	else
		m_chains->save_config(parentnode);
	// All sliders are saved by chain_manager's standard <screen> persistence.
}
