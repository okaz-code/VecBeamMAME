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
#include "video/vector.h"
#include "rendutil.h"

// complete slider_state type (for core->description access)
#include "../frontend/mame/ui/slider.h"

// util
#include "util/xmlfile.h"

// OSD
#include "modules/lib/osdobj_common.h"
#include "window.h"

#include <bx/math.h>
#include <bx/readerwriter.h>

#if defined(SDLMAME_WIN32) || defined(OSD_WINDOWS)
// standard windows headers
#include <windows.h>
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
#endif

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include "imgui/imgui.h"

#include <algorithm>
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


//============================================================
//  video_bgfx::init_bgfx_library
//============================================================

bool video_bgfx::init_bgfx_library(osd_window &window)
{
	osd_dim const wdim = window.get_size_pixels();

	bgfx::Init init;
	init.type = bgfx::RendererType::Count;
	init.vendorId = BGFX_PCI_ID_NONE;
	init.resolution.width = wdim.width();
	init.resolution.height = wdim.height();
	init.resolution.numBackBuffers = 1;
	init.resolution.reset = video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
	// HDR PoC: request an HDR10 swapchain (PQ / Rec.2020 with an RGB10A2 backbuffer). bgfx only
	// raises BGFX_CAPS_HDR10 when the OS output is already in HDR mode (d3d11/d3d12), checked
	// after init below.
	if (m_options->bgfx_hdr())
	{
		s_bgfx_hdr_active = true;
		init.resolution.reset |= BGFX_RESET_HDR10;
		init.resolution.format = bgfx::TextureFormat::RGB10A2;
	}
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

	if (!bgfx::init(init))
		return false;

	// HDR PoC: fall back to SDR when the device/output cannot do HDR10 (Windows HDR off,
	// non-d3d11/12 backend, SDR monitor).
	if (s_bgfx_hdr_active && (bgfx::getCaps()->supported & BGFX_CAPS_HDR10) == 0)
	{
		osd_printf_warning("BGFX: HDR10 requested but not available (is Windows HDR on, backend d3d11/d3d12?), falling back to SDR\n");
		s_bgfx_hdr_active = false;
	}

	bgfx::reset(wdim.width(), wdim.height(),
		(video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) | (s_bgfx_hdr_active ? BGFX_RESET_HDR10 : 0),
		s_bgfx_hdr_active ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::Count);

	// Enable debug text if requested
	bool bgfx_debug = m_options->bgfx_debug();
	bgfx::setDebug(bgfx_debug ? BGFX_DEBUG_STATS : BGFX_DEBUG_TEXT);

	// Get actual maximum texture size
	bgfx::Caps const *const caps = bgfx::getCaps();
	m_max_texture_size = caps->limits.maxTextureSize;

	ScreenVertex::init();
	AnalyticLineVertex::init();

	imguiCreate();

	return true;
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
	m_sliders_dirty = true;

	uint32_t flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
	m_texture_cache = m_textures->create_texture("#cache", bgfx::TextureFormat::BGRA8, CACHE_SIZE, 0, CACHE_SIZE, nullptr, flags);

	memset(m_white, 0xff, sizeof(uint32_t) * 16 * 16);
	m_texinfo.push_back(rectangle_packer::packable_rectangle(WHITE_HASH, PRIMFLAG_TEXFORMAT(TEXFORMAT_ARGB32), 16, 16, 16, nullptr, m_white));

	// Create the vector-drawing FBO (window 0 only).
	// Managed purely through the BGFX API, not via chain_manager / target_manager.
	// Use two attachments, color + depth (BGFX sometimes assumes a depth attachment exists when
	// drawing). Follows how bgfx_target.cpp creates them.
	if (window().index() == 0)
	{
		const int ss = m_module().options().bgfx_vec_supersample();
		m_vec_supersample = uint16_t(ss < 1 ? 1 : (ss > 2 ? 2 : ss));
		m_vec_fb_w = uint16_t(wdim.width() * m_vec_supersample);
		m_vec_fb_h = uint16_t(wdim.height() * m_vec_supersample);
		// draw() recreates this when the size no longer matches (only the initial creation is here;
		// draw()'s recreation logic uses equivalent code).
		// POINT filter flags omitted -> default bilinear.
		// MSAA can't be sampled by a standard sampler, so it's avoided; supersample + fs_vector_line instead.
		const uint64_t color_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
			BGFX_TEXTURE_RT;
		const uint64_t depth_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
			BGFX_TEXTURE_RT;
		// RG11B10F: same 32bpp as BGRA8 but float - additive blending no longer clamps at 1.0,
		// so line crossings and end caps keep their real energy for the chain (and for HDR).
		bgfx::TextureHandle tex_color = bgfx::createTexture2D(
			m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, color_flags);
		bgfx::TextureHandle tex_depth = bgfx::createTexture2D(
			m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::D32F, depth_flags);
		bgfx::TextureHandle attachments[2] = { tex_color, tex_depth };
		m_vec_fb = bgfx::createFrameBuffer(2, attachments, true);  // true=textures owned by FBO

		// Analytic-glow FBO: colour-only, same size/format (additive draw, no depth needed).
		bgfx::TextureHandle glow_color = bgfx::createTexture2D(
			m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, color_flags);
		m_vec_glow_fb = bgfx::createFrameBuffer(1, &glow_color, true);
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

		// Subscribe to the vector device's beam notifiers for the monitor-glow effect.
		// frame_begin resets the per-frame accumulator; the beam-energy-line notifier accumulates
		// off-screen beam energy weighted by how far the beam runs past the screen edge. The
		// thresholds/coefficient come from the active chain's sliders, so a chain that does not
		// define them (coefficient defaults to 0) accumulates nothing.
		for (vector_device &vec : device_type_enumerator<vector_device>(window().machine().root_device()))
		{
			m_vector_device = &vec;  // for the CRT-flicker stale-frame query
			// Opt in to beam-event mode: timed points are consumed once presented, so each frame
			// shows only the lines the beam actually drew during it (per-vector CRT flicker).
			// Untimed vector sources (non-avgdvg drivers) keep stock list semantics.
			// -bgfx_vec_beam_events 0 restores the classic whole-list behaviour entirely.
			m_vec_beam_events = m_module().options().bgfx_vec_beam_events();
			vec.set_beam_event_mode(m_vec_beam_events);
			m_mglow_frame_sub = vec.add_frame_begin_notifier([this] () { m_mglow_amount = 0.0f; m_hv_energy = 0.0f; m_vec_new_frame = true; });
			m_mglow_line_sub = vec.add_beam_energy_line_notifier(
				[this] (float x0, float y0, float x1, float y1, float beam_energy)
				{
					// HV supply droop load: total beam energy = current (beam_energy) x draw time
					// (proportional to length at constant velocity), summed over the whole frame.
					if (beam_energy > 0.0f)
					{
						const float lx = x1 - x0, ly = y1 - y0;
						m_hv_energy += beam_energy * sqrtf(lx * lx + ly * ly);
					}
					const float coeff = m_chains->slider_value(0, "mglow_coefficient", 0.0f);
					const float thr   = m_chains->slider_value(0, "mglow_threshold", 0.7f);
					if (coeff <= 0.0f || beam_energy <= thr)
						return;
					const float mind = m_chains->slider_value(0, "mglow_min_distance", 0.30f);
					auto outside = [] (float x, float y) {
						const float dx = (x < 0.0f) ? -x : (x > 1.0f) ? (x - 1.0f) : 0.0f;
						const float dy = (y < 0.0f) ? -y : (y > 1.0f) ? (y - 1.0f) : 0.0f;
						return std::max(dx, dy);
					};
					if (std::max(outside(x0, y0), outside(x1, y1)) > mind)
						m_mglow_amount += (beam_energy - thr) * coeff;
				});
			break;
		}
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
	m_avi_view->set_index(s_current_view);
	m_avi_view->setup();

	bgfx::setViewRect(s_current_view, 0, 0, s_width[0], s_height[0]);
	bgfx::setViewClear(s_current_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);

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

	uint32_t blend = PRIMFLAG_GET_BLENDMODE(prim->flags);
	bgfx::setVertexBuffer(0,buffer);
	bgfx::setTexture(0, effects[blend]->uniform("s_tex")->handle(), texture);

	bgfx_uniform* inv_view_dims = effects[blend]->uniform("u_inv_view_dims");
	if (inv_view_dims)
	{
		float values[2] = { -1.0f / s_width[window_index], 1.0f / s_height[window_index] };
		inv_view_dims->set(values, sizeof(float) * 2);
		inv_view_dims->upload();
	}

	effects[blend]->submit(m_ortho_view->get_index());

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

// Normalized tunable sigmoid (Dino Dini's curve, n and k in [-1, 1]); k=0 is linear.
// Used by the renderer-side overload model to shape the display/width response.
static float vector_overload_sigmoid(float n, float k)
{
	return (n - n * k) / (k - fabsf(n) * 2.0f * k + 1.0f);
}

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
	const float strength = std::clamp(m_chains->slider_value(0, "deflection_dynamics", 0.0f), 0.0f, 1.0f);
	const float settle_us = std::max(0.1f, m_chains->slider_value(0, "deflection_settle", 5.0f));
	const float zeta     = std::clamp(m_chains->slider_value(0, "deflection_damping", 0.5f), 0.05f, 2.0f);
	const float res      = float(s_width[window().index()]) / 1920.0f;

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

void renderer_bgfx::put_analytic_line(render_primitive *prim, AnalyticLineVertex *vertex, AnalyticLineVertex *glow_vertex, float start_cap, float end_cap)
{
	float x0 = prim->bounds.x0 + m_vec_drift_x, y0 = prim->bounds.y0 + m_vec_drift_y;
	float x1 = prim->bounds.x1 + m_vec_drift_x, y1 = prim->bounds.y1 + m_vec_drift_y;

	// Vector linearity calibration (board "Linear" / X-Y SIZE pots = per-axis integrator gain): draw
	// each vector as the commanded delta x gain (X and Y independent), continuing from where the beam
	// actually ended up. A gain != 1 makes a contiguous stroke grow/shrink and the error accumulate
	// along it, resetting at a jump (a start that does not meet the previous commanded end) or a new
	// frame. 1.0 / 1.0 = exact (off).
	const float lx = m_chains->slider_value(0, "vector_linearity_x", 1.0f);
	const float ly = m_chains->slider_value(0, "vector_linearity_y", 1.0f);
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

	// Same renderer-side overload model as put_solid_line (width / display intensity / overload).
	const float ov_threshold = m_chains->slider_value(0, "overload_threshold", -1.0f);
	const bool  overload_chain = (ov_threshold >= 0.0f);
	const float point_threshold = m_chains->slider_value(0, "line_point_threshold", LINE_POINT_THRESHOLD);
	const bool as_point = (seg_len <= point_threshold);

	float width;
	float display_a;
	float ovld = 0.0f;
	if (overload_chain)
	{
		const float src = std::clamp(prim->beam_energy, 0.0f, 1.0f);
		const float k   = m_chains->slider_value(0, "overload_sigmoid_k", 0.0f);
		const float thr = std::clamp(ov_threshold, 0.001f, 1.0f);
		const float out = std::clamp(vector_overload_sigmoid(src, k), 0.0f, 1.0f);
		const float bw_min = m_chains->slider_value(0, "beam_width_min", 1.0f);
		const float bw_max = m_chains->slider_value(0, "beam_width_max", 1.5f);
		const float ow_max = m_chains->slider_value(0, "overload_width_max", 5.0f);
		float beam_units;
		if (out <= thr)
		{
			const float t = out / thr;
			display_a = t;
			const float w = std::clamp(vector_overload_sigmoid(t,
				m_chains->slider_value(0, "overload_width_curve_low", 0.0f)), 0.0f, 1.0f);
			beam_units = bw_min + w * (bw_max - bw_min);
		}
		else
		{
			display_a = 1.0f;
			const float span = std::max(0.001f, 1.0f - thr);
			ovld = std::clamp((out - thr) / span, 0.0f, 1.0f);
			const float w = std::clamp(vector_overload_sigmoid(ovld,
				m_chains->slider_value(0, "overload_width_curve_high", 0.0f)), 0.0f, 1.0f);
			beam_units = bw_max + w * (ow_max - bw_max);
		}
		if (as_point)
		{
			const float dmin = m_chains->slider_value(0, "beam_dot_size_min", 0.1f);
			const float dmax = m_chains->slider_value(0, "beam_dot_size_max", 0.5f);
			const float w = std::clamp(vector_overload_sigmoid(src,
				m_chains->slider_value(0, "beam_dot_size_curve", 0.0f)), 0.0f, 1.0f);
			beam_units = std::max(0.0f, dmin + w * (dmax - dmin));
		}
		width = beam_units * (float(s_width[window().index()]) / 1920.0f);
	}
	else
	{
		display_a = prim->color.a;
		// Non-overload chains: if they define beam_width sliders (vector-color / monochrome), let
		// those drive the width too (interpolated by the line's display intensity), the same way
		// the overload chain does - not just the ini beam_width options. Chains without the slider
		// (CRT etc.) keep the stock prim->width.
		const float bw_min = m_chains->slider_value(0, "beam_width_min", -1.0f);
		if (bw_min >= 0.0f)
		{
			const float bw_max = m_chains->slider_value(0, "beam_width_max", bw_min);
			const float intensity = std::clamp(prim->color.a, 0.0f, 1.0f);
			float beam_units = bw_min + intensity * (bw_max - bw_min);
			if (as_point)
			{
				const float dmin = m_chains->slider_value(0, "beam_dot_size_min", 0.1f);
				const float dmax = m_chains->slider_value(0, "beam_dot_size_max", 0.5f);
				const float w = std::clamp(vector_overload_sigmoid(intensity,
					m_chains->slider_value(0, "beam_dot_size_curve", 0.0f)), 0.0f, 1.0f);
				beam_units = std::max(0.0f, dmin + w * (dmax - dmin));
			}
			width = beam_units * (float(s_width[window().index()]) / 1920.0f);
		}
		else
		{
			width = prim->width;
		}
	}
	if (width < 0.5f) width = 0.5f;

	// Length fade + flicker + window-blend weight, identical to the classic path.
	const float vls_scale = m_chains->slider_value(0, "vector_length_scale", 0.0f);
	const float vls_ratio = m_chains->slider_value(0, "vector_length_ratio", 0.5f);
	float length_factor = 1.0f;
	if (vls_scale > 0.0001f && vls_ratio > 0.0001f)
	{
		const float screen_ref = float(std::max(s_width[window().index()], s_height[window().index()]));
		const float norm_len = (screen_ref > 0.0f) ? (seg_len / screen_ref) : 0.0f;
		const float length_modulate = 1.0f - std::min(norm_len / vls_ratio, 1.0f);
		length_factor = 1.0f + vls_scale * (length_modulate - 1.0f);
	}
	length_factor *= m_crt_flicker_factor * m_vec_line_weight;

	// Dwell-time brightness (real DVG behaviour, per jmargolin.com/vgens): a vector is drawn in a
	// roughly length-independent time, so a shorter/slower-drawn beam concentrates its energy over
	// a shorter path and glows brighter - dots (near-zero length) brightest. Uses the per-vector
	// draw interval (t1-t0) carried by the timestamped beam events. dwell_brightness 0 = off, so
	// chains without the slider (and untimed sources) are unaffected.
	const float dwell_str = m_chains->slider_value(0, "dwell_brightness", 0.0f);
	if (dwell_str > 0.0f && prim->t0 >= 0.0 && prim->t1 > prim->t0)
	{
		const float dmax = m_chains->slider_value(0, "dwell_max", 3.0f);
		const float screen_ref = float(std::max(s_width[window().index()], s_height[window().index()]));
		const float norm_len = (screen_ref > 0.0f) ? (seg_len / screen_ref) : 0.0f;
		const double dt_ms = (prim->t1 - prim->t0) * 1000.0;
		float mul = dmax;
		if (norm_len > 1e-4f && dt_ms > 0.0)
		{
			// draw speed in screen-fractions per millisecond; brightness ~ ref / speed
			const float v = norm_len / float(dt_ms);
			const float ref = m_chains->slider_value(0, "dwell_speed_ref", 1.0f);
			mul = std::clamp((v > 1e-6f) ? (ref / v) : dmax, 0.3f, dmax);
		}
		length_factor *= (1.0f + dwell_str * (mul - 1.0f));
	}

	// HV supply droop (master plan 3-4 / 6.2): a bright/busy frame sags the EHT supply, dimming the
	// whole picture (here) and defocusing the spot (sigma, below). m_hv_load_norm is the smoothed 0..1
	// frame load; hv_droop scales the effect (0 = off). The dim is capped at 0.4 of full brightness.
	const float hv_droop = m_chains->slider_value(0, "hv_droop", 0.0f);
	if (hv_droop > 0.0f && m_hv_load_norm > 0.0f)
		length_factor *= (1.0f - hv_droop * 0.4f * m_hv_load_norm);

	// clamp: length_factor can exceed 1.0 with the dwell-time boost, and u32Color does not clamp
	const uint32_t rgba = u32Color(
		std::min<uint32_t>(uint32_t(prim->color.r * length_factor * 255.0f + 0.5f), 255),
		std::min<uint32_t>(uint32_t(prim->color.g * length_factor * 255.0f + 0.5f), 255),
		std::min<uint32_t>(uint32_t(prim->color.b * length_factor * 255.0f + 0.5f), 255),
		uint32_t(std::clamp(display_a, 0.0f, 1.0f) * 255.0f + 0.5f));

	// sigma: width/3.2 keeps the gaussian core as tight as the classic parabola (a gaussian's
	// tails read as soft focus at equal FWHM); the overload defocus widens it (the classic
	// path's parabola->gaussian blend reached about 2x at full overload).
	float sigma = (width / 3.2f) * (1.0f + ovld);
	// Intensity-driven blooming (vgens: overdriving the beam draws more current, sagging the HV
	// and defocusing the spot - a brighter line/dot has a physically wider core). sigma grows with
	// the rendered peak brightness, which already folds in the dwell-time boost, so the brightest
	// dots (bullets) bloom most. Applied only to the core spot here, not the caps/ring. The slider
	// strength is in 1920-reference pixels (scaled to the actual resolution). beam_bloom_strength
	// 0 = off, so chains without the slider are unaffected.
	const float beam_bloom = m_chains->slider_value(0, "beam_bloom_strength", 0.0f);
	if (beam_bloom > 0.0f)
	{
		const float bloom_p = m_chains->slider_value(0, "beam_bloom_curve", 1.0f);
		const float bI = std::max(std::max(prim->color.r, prim->color.g), prim->color.b) * length_factor;
		if (bI > 0.0f)
		{
			const float bres = float(s_width[window().index()]) / 1920.0f;
			sigma += beam_bloom * bres * powf(std::min(bI, 4.0f), bloom_p);
		}
	}
	// Edge defocus (vgens / master plan 3-5): at large deflection angles the spot defocuses
	// astigmatically, so sigma grows toward the screen edges. The segment midpoint's radius is
	// normalised to the half-diagonal (0 at centre, 1 at a corner) and raised to edge_defocus_curve
	// (2 = quadratic, matching deflection-angle growth). edge_defocus 0 = off.
	const float edge_def = m_chains->slider_value(0, "edge_defocus", 0.0f);
	if (edge_def > 0.0f)
	{
		const float sw = float(s_width[window().index()]);
		const float sh = float(s_height[window().index()]);
		const float halfdiag = 0.5f * sqrtf(sw * sw + sh * sh);
		const float mx = (x0 + x1) * 0.5f - sw * 0.5f;
		const float my = (y0 + y1) * 0.5f - sh * 0.5f;
		float r = (halfdiag > 0.0f) ? std::min(sqrtf(mx * mx + my * my) / halfdiag, 1.0f) : 0.0f;
		const float ecurve = m_chains->slider_value(0, "edge_defocus_curve", 2.0f);
		sigma += edge_def * (sw / 1920.0f) * powf(r, ecurve);
	}
	// HV droop defocus: the same supply sag that dims the picture widens the spot (capped ~2.5 px at
	// 1920-ref, scaled by the load). Pairs with the dim applied to length_factor above.
	if (hv_droop > 0.0f && m_hv_load_norm > 0.0f)
		sigma += hv_droop * 2.5f * (float(s_width[window().index()]) / 1920.0f) * m_hv_load_norm;
	// Rasterization floor so a sub-pixel gaussian does not fall between fragment centres and
	// vanish. Lines are 1D-continuous so they can go thinner than points (which have no extent
	// in either axis and need a wider floor). 0.33 sigma = FWHM ~0.78px, about the thinnest a
	// vector line stays solid - this is the practical minimum the beam_width slider reaches.
	const float sig_floor = as_point ? 0.85f : 0.33f;
	if (sigma < sig_floor) sigma = sig_floor;
	const float pad = 3.5f * sigma + 0.5f;

	if (seg_len > 0.0001f) { const float inv = 1.0f / seg_len; dx *= inv; dy *= inv; }
	else { dx = 1.0f; dy = 0.0f; }
	const float nx = dy, ny = -dx;

	auto setv = [&](int i, float x, float y, float a, float b, float d, float sg) {
		vertex[i].m_x = x; vertex[i].m_y = y; vertex[i].m_z = 0.0f;
		vertex[i].m_rgba = rgba;
		vertex[i].m_a = a; vertex[i].m_b = b; vertex[i].m_d = d; vertex[i].m_sigma = sg;
	};

	// 2D gaussian dot quad (point mode: sigma sign flags it in the shader)
	auto set_dot = [&](int base, float cx, float cy, float sg_abs, uint32_t drgba) {
		const float p = 3.5f * sg_abs + 0.5f;
		const float sg = -sg_abs;
		auto dv = [&](int i, float x, float y, float a, float d) {
			vertex[i].m_x = x; vertex[i].m_y = y; vertex[i].m_z = 0.0f;
			vertex[i].m_rgba = drgba;
			vertex[i].m_a = a; vertex[i].m_b = 0.0f; vertex[i].m_d = d; vertex[i].m_sigma = sg;
		};
		dv(base + 0, cx - p, cy - p, -p, -p);
		dv(base + 1, cx + p, cy - p,  p, -p);
		dv(base + 2, cx + p, cy + p,  p,  p);
		dv(base + 3, cx - p, cy - p, -p, -p);
		dv(base + 4, cx + p, cy + p,  p,  p);
		dv(base + 5, cx - p, cy + p, -p,  p);
	};
	auto set_degenerate = [&](int base) {
		for (int i = 0; i < 6; i++)
		{
			vertex[base + i].m_x = x0; vertex[base + i].m_y = y0; vertex[base + i].m_z = 0.0f;
			vertex[base + i].m_rgba = 0;
			vertex[base + i].m_a = 0.0f; vertex[base + i].m_b = 0.0f; vertex[base + i].m_d = 0.0f; vertex[base + i].m_sigma = -1.0f;
		}
	};

	// Halation ring: one smooth circle centred on the dot (b = radius flags ring mode in the
	// shader; sigma = -edge width). A single quad per bullet, so it is continuous, not a ring of
	// gather dots.
	auto set_ring = [&](int base, float cx, float cy, float radius, float width, uint32_t rrgba) {
		const float p = radius + 3.0f * width + 1.0f;  // quad half-extent covers the soft rim
		const float sg = -width;
		auto rv = [&](int i, float x, float y, float a, float d) {
			vertex[i].m_x = x; vertex[i].m_y = y; vertex[i].m_z = 0.0f;
			vertex[i].m_rgba = rrgba;
			vertex[i].m_a = a; vertex[i].m_b = radius; vertex[i].m_d = d; vertex[i].m_sigma = sg;
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
	const float glow_str  = m_chains->slider_value(0, "analytic_glow", 0.0f);
	const float glow_w    = m_chains->slider_value(0, "analytic_glow_width", 8.0f) * (float(s_width[window().index()]) / 1920.0f);
	const float glow_sig  = sigma + std::max(0.0f, glow_w);
	// Glow onset: only sources brighter than glow_threshold glow, ramped by glow_curve - so faint
	// stars stay dark while bright bullets/explosions bloom. glow_threshold 0 + glow_curve 1 reproduce
	// the old linear behaviour (magnitude = colour x length_factor x analytic_glow) exactly. The hue is
	// preserved (colour normalised by its peak) and the magnitude carries the shaped intensity.
	const float glow_thr  = m_chains->slider_value(0, "glow_threshold", 0.0f);
	const float glow_crv  = m_chains->slider_value(0, "glow_curve", 1.0f);
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
		// dwelling beam: one 2D gaussian at the segment centre
		set_dot(0, cx, cy, sigma, rgba);

		// Halation around bright dwell dots (bullets). The rendered brightness includes the
		// dwell-time boost, so only the bright dots reach the threshold; rocks/ship lines never
		// take this branch. The rim (gain) and the inner fill have independent brightness so the
		// fill stays visible when the rim is dialed right down.
		const float ring_gain = m_chains->slider_value(0, "ring_gain", 0.0f);
		const float ring_fill = m_chains->slider_value(0, "ring_fill", 0.0f);
		const float eff_bright = std::max(std::max(prim->color.r, prim->color.g), prim->color.b) * length_factor;
		const bool ring_on = (ring_gain > 0.0f || ring_fill > 0.0f)
			&& eff_bright >= m_chains->slider_value(0, "ring_threshold", 0.0f);
		const float res = float(s_width[window().index()]) / 1920.0f;
		const float radius = std::max(2.0f, m_chains->slider_value(0, "ring_radius", 24.0f) * res);
		const float da = std::clamp(display_a, 0.0f, 1.0f);
		auto ring_color = [&](float strength) -> uint32_t {
			return u32Color(
				std::min<uint32_t>(uint32_t(prim->color.r * length_factor * strength * 255.0f + 0.5f), 255),
				std::min<uint32_t>(uint32_t(prim->color.g * length_factor * strength * 255.0f + 0.5f), 255),
				std::min<uint32_t>(uint32_t(prim->color.b * length_factor * strength * 255.0f + 0.5f), 255),
				std::min<uint32_t>(uint32_t(da * 255.0f + 0.5f), 255));
		};

		// rim: gaussian ring at R0 (internal 0.05 keeps the gain slider gentle)
		if (ring_on && ring_gain > 0.0f)
		{
			const float width = std::max(0.75f, m_chains->slider_value(0, "ring_width", 3.0f) * res);
			set_ring(6, cx, cy, radius, width, ring_color(ring_gain * 0.05f));
		}
		else
			set_degenerate(6);

		// inner fill: a soft gaussian disc inside the ring, brightness independent of the rim gain
		if (ring_on && ring_fill > 0.0f)
			set_dot(12, cx, cy, std::max(1.0f, radius * 0.5f), ring_color(ring_fill * 0.04f));
		else
			set_degenerate(12);
		// In deflection mode the line buffer is wider (DEFL_NOUT body quads + 2 caps); a point only
		// uses groups 0..2 (dot/ring/fill), so blank the rest.
		if (m_defl_on)
			for (int g = 3; g * 6 < int(m_vec_vpl); g++)
				set_degenerate(g * 6);
		// glow: a wide gaussian dot around the dwell point, into the separate glow buffer
		if (glow_vertex)
		{
			const float gp = 3.5f * glow_sig + 0.5f;
			const float gs = -glow_sig;
			auto gdv = [&](int i, float x, float y, float a, float d) {
				glow_vertex[i].m_x = x; glow_vertex[i].m_y = y; glow_vertex[i].m_z = 0.0f; glow_vertex[i].m_rgba = glow_rgba;
				glow_vertex[i].m_a = a; glow_vertex[i].m_b = 0.0f; glow_vertex[i].m_d = d; glow_vertex[i].m_sigma = gs;
			};
			gdv(0, cx - gp, cy - gp, -gp, -gp);
			gdv(1, cx + gp, cy - gp,  gp, -gp);
			gdv(2, cx + gp, cy + gp,  gp,  gp);
			gdv(3, cx - gp, cy - gp, -gp, -gp);
			gdv(4, cx + gp, cy + gp,  gp,  gp);
			gdv(5, cx - gp, cy + gp, -gp,  gp);
		}
		return;
	}

	// End caps: gaussian dots driven by the same line_cap sliders as the classic path
	// (size/min/intensity-curve/brightness). The erf already gives the physical 50% end
	// roll-off; these add the visible bright endpoint on top, until the dwell-time model
	// (master plan 2-3) replaces them. In deflection mode the body uses DEFL_NOUT quads, so the
	// caps move to the slots right after it.
	const int cap0 = m_defl_on ? DEFL_NOUT * 6 : 6;
	const int cap1 = cap0 + 6;
	{
		const float cap_res_scale = float(s_width[window().index()]) / 1920.0f;
		const float cap_full   = m_chains->slider_value(0, "line_cap_size", LINE_CAP_SIZE_PX) * cap_res_scale;
		const float cap_min_px = m_chains->slider_value(0, "line_cap_min_size", 0.0f) * cap_res_scale;
		const float cap_curve  = m_chains->slider_value(0, "line_cap_intensity_curve", 0.0f);
		const float cap_bi     = std::clamp(prim->color.a, 0.0f, 1.0f);
		const float cap_f      = (cap_curve <= 0.0001f) ? 1.0f : powf(cap_bi, cap_curve);
		const float cap_radius = std::max(0.0f, cap_min_px + (cap_full - cap_min_px) * cap_f);
		if (cap_radius > 0.05f)
		{
			const float cap_bright = std::max(0.0f, m_chains->slider_value(0, "line_cap_brightness", 1.0f));
			// The cap dot is ADDED on top of the line's erf end (~50% at the endpoint). At full line
			// intensity that made the vertex ~1.5x the body, and the brighter pixel lingered longer
			// under the phosphor max()-persistence than the moving line - a lagging vertex trail on
			// rotating shapes. Scale the cap contribution to ~0.5x so the endpoint peak lands near the
			// line intensity (0.5 erf + 0.5 cap). line_cap_brightness still boosts from there.
			const float cap_scale = 0.5f * cap_bright;
			// Per-endpoint dwell factor (master plan 2-3, vertex dwell): start_cap / end_cap come from
			// the neighbour-aware pre-pass - 1.0 at stroke termini and sharp corners (the beam dwells),
			// down toward 0 at straight joints (no dwell). vertex_dwell 0 leaves both at 1.0 = the old
			// uniform cap. The dot brightness scales with it; a 0 factor yields a zero-colour (skipped) dot.
			auto cap_rgba_for = [&](float boost) -> uint32_t {
				const float s = cap_scale * boost;
				return u32Color(
					std::min<uint32_t>(uint32_t(prim->color.r * length_factor * s * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(prim->color.g * length_factor * s * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(prim->color.b * length_factor * s * 255.0f + 0.5f), 255),
					std::min<uint32_t>(uint32_t(std::clamp(display_a, 0.0f, 1.0f) * 255.0f + 0.5f), 255));
			};
			const float sg_cap = std::max(0.85f, cap_radius * 0.6f);
			set_dot(cap0, x0, y0, sg_cap, cap_rgba_for(start_cap));
			set_dot(cap1, x1, y1, sg_cap, cap_rgba_for(end_cap));
		}
		else
		{
			set_degenerate(cap0);
			set_degenerate(cap1);
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
		const float gpad = 3.5f * glow_sig + 0.5f;
		const float gsx0 = x0 - dx * gpad, gsy0 = y0 - dy * gpad;
		const float gsx1 = x1 + dx * gpad, gsy1 = y1 + dy * gpad;
		const float ga0 = -gpad, ga1 = seg_len + gpad;
		auto gv = [&](int i, float x, float y, float a, float b, float d) {
			glow_vertex[i].m_x = x; glow_vertex[i].m_y = y; glow_vertex[i].m_z = 0.0f; glow_vertex[i].m_rgba = glow_rgba;
			glow_vertex[i].m_a = a; glow_vertex[i].m_b = b; glow_vertex[i].m_d = d; glow_vertex[i].m_sigma = glow_sig;
		};
		gv(0, gsx0 + nx * gpad, gsy0 + ny * gpad, ga0, ga0 - seg_len,  gpad);
		gv(1, gsx1 + nx * gpad, gsy1 + ny * gpad, ga1, ga1 - seg_len,  gpad);
		gv(2, gsx1 - nx * gpad, gsy1 - ny * gpad, ga1, ga1 - seg_len, -gpad);
		gv(3, gsx0 + nx * gpad, gsy0 + ny * gpad, ga0, ga0 - seg_len,  gpad);
		gv(4, gsx1 - nx * gpad, gsy1 - ny * gpad, ga1, ga1 - seg_len, -gpad);
		gv(5, gsx0 - nx * gpad, gsy0 - ny * gpad, ga0, ga0 - seg_len, -gpad);
	}
}

void renderer_bgfx::put_solid_line(render_primitive *prim, ScreenVertex* vertex)
{
	float x0 = prim->bounds.x0 + m_vec_drift_x;
	float y0 = prim->bounds.y0 + m_vec_drift_y;
	float x1 = prim->bounds.x1 + m_vec_drift_x;
	float y1 = prim->bounds.y1 + m_vec_drift_y;

	float dx = x1 - x0;
	float dy = y1 - y0;
	const float seg_len = sqrtf(dx * dx + dy * dy);

	// Renderer-side overload model. Consumes prim->beam_energy (normalized 0..1 beam energy supplied
	// by the vector device) and the chain's overload sliders to derive the display intensity, the
	// beam width and the overload amount. This is the math the emulation core used to do; keeping it
	// here leaves the device renderer-agnostic. Chains without an "overload_threshold" slider opt out
	// (overload_chain == false) and keep the stock width/intensity, so CRT chains that do not define
	// these sliders are unaffected.
	const float ov_threshold = m_chains->slider_value(0, "overload_threshold", -1.0f);
	const bool  overload_chain = (ov_threshold >= 0.0f);
	const float point_threshold = m_chains->slider_value(0, "line_point_threshold", LINE_POINT_THRESHOLD);
	// Point-treatment test: short segments (add_point gives x0==x1,y0==y1 -> seg_len 0) are drawn as a
	// single circle so the two half-circle caps do not overlap into a double-bright distorted blob.
	const bool as_point = (seg_len <= point_threshold);

	float width;
	float display_a;    // line display intensity (0..1), written to the vertex alpha
	float ovld = 0.0f;  // overload amount (0..1) handed to fs_vector_line via m_u for the defocus

	if (overload_chain)
	{
		const float src = std::clamp(prim->beam_energy, 0.0f, 1.0f);
		const float k   = m_chains->slider_value(0, "overload_sigmoid_k", 0.0f);
		const float thr = std::clamp(ov_threshold, 0.001f, 1.0f);
		const float out = std::clamp(vector_overload_sigmoid(src, k), 0.0f, 1.0f);
		const float bw_min = m_chains->slider_value(0, "beam_width_min", 1.0f);
		const float bw_max = m_chains->slider_value(0, "beam_width_max", 1.5f);
		const float ow_max = m_chains->slider_value(0, "overload_width_max", 5.0f);

		float beam_units;
		if (out <= thr)
		{
			// below the threshold: ramp display 0..1 and width min..max via the low curve
			const float t = out / thr;
			display_a = t;
			const float w = std::clamp(vector_overload_sigmoid(t,
				m_chains->slider_value(0, "overload_width_curve_low", 0.0f)), 0.0f, 1.0f);
			beam_units = bw_min + w * (bw_max - bw_min);
		}
		else
		{
			// above the threshold: display clips to full, width grows max..overload_max, beam overloads
			display_a = 1.0f;
			const float span = std::max(0.001f, 1.0f - thr);
			ovld = std::clamp((out - thr) / span, 0.0f, 1.0f);
			const float w = std::clamp(vector_overload_sigmoid(ovld,
				m_chains->slider_value(0, "overload_width_curve_high", 0.0f)), 0.0f, 1.0f);
			beam_units = bw_max + w * (ow_max - bw_max);
		}

		// points use a dedicated dot size interpolated by the source value (independent of width)
		if (as_point)
		{
			const float dmin = m_chains->slider_value(0, "beam_dot_size_min", 0.1f);
			const float dmax = m_chains->slider_value(0, "beam_dot_size_max", 0.5f);
			const float w = std::clamp(vector_overload_sigmoid(src,
				m_chains->slider_value(0, "beam_dot_size_curve", 0.0f)), 0.0f, 1.0f);
			beam_units = std::max(0.0f, dmin + w * (dmax - dmin));
		}

		// beam_units are pixel widths at a 1920px-wide window; scale to the current resolution.
		width = beam_units * (float(s_width[window().index()]) / 1920.0f);
	}
	else
	{
		display_a = prim->color.a;
		// Non-overload chains: honour their beam_width / beam_dot sliders if present (vector-color /
		// monochrome), interpolated by display intensity, so they work like the overload chain.
		const float bw_min = m_chains->slider_value(0, "beam_width_min", -1.0f);
		if (bw_min >= 0.0f)
		{
			const float bw_max = m_chains->slider_value(0, "beam_width_max", bw_min);
			const float intensity = std::clamp(prim->color.a, 0.0f, 1.0f);
			float beam_units = bw_min + intensity * (bw_max - bw_min);
			if (as_point)
			{
				const float dmin = m_chains->slider_value(0, "beam_dot_size_min", 0.1f);
				const float dmax = m_chains->slider_value(0, "beam_dot_size_max", 0.5f);
				const float w = std::clamp(vector_overload_sigmoid(intensity,
					m_chains->slider_value(0, "beam_dot_size_curve", 0.0f)), 0.0f, 1.0f);
				beam_units = std::max(0.0f, dmin + w * (dmax - dmin));
			}
			width = beam_units * (float(s_width[window().index()]) / 1920.0f);
		}
		else
		{
			width = prim->width;
		}
	}
	if (width < 0.5f) width = 0.5f;

	// Length fade (port of the HLSL vector.fx length modulation): longer segments lose intensity,
	// reproducing the electron-beam current load. Folded into the line colour here.
	//   norm_len = pixel_len / max(window_w, window_h)
	//   length_factor = lerp(1, 1 - clamp(norm_len / ratio, 0, 1), scale)
	const float vls_scale = m_chains->slider_value(0, "vector_length_scale", 0.0f);
	const float vls_ratio = m_chains->slider_value(0, "vector_length_ratio", 0.5f);
	float length_factor = 1.0f;
	if (vls_scale > 0.0001f && vls_ratio > 0.0001f)
	{
		const float screen_ref = float(std::max(s_width[window().index()], s_height[window().index()]));
		const float norm_len = (screen_ref > 0.0f) ? (seg_len / screen_ref) : 0.0f;
		const float length_modulate = 1.0f - std::min(norm_len / vls_ratio, 1.0f);
		length_factor = 1.0f + vls_scale * (length_modulate - 1.0f);
	}
	// Fold in the per-frame CRT-flicker dim (1.0 when not flickering) and the time-window
	// energy weight of this line (< 1.0 only in the window-boundary blend zone).
	length_factor *= m_crt_flicker_factor * m_vec_line_weight;

	// Pack the line color: hue from the primitive (× length fade × flicker), alpha = display intensity.
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
	const float cap_bright = std::max(0.0f, m_chains->slider_value(0, "line_cap_brightness", 1.0f));
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
	const float cap_res_scale = float(s_width[window().index()]) / 1920.0f;
	// Cap radius interpolates line_cap_min_size..line_cap_size by line intensity (prim->color.a) via
	// the Line Cap Intensity Curve (pow exponent); curve 0 (default) keeps the full size for every line.
	const float cap_full   = m_chains->slider_value(0, "line_cap_size", LINE_CAP_SIZE_PX) * cap_res_scale;
	const float cap_min_px = m_chains->slider_value(0, "line_cap_min_size", 0.0f) * cap_res_scale;
	const float cap_curve  = m_chains->slider_value(0, "line_cap_intensity_curve", 0.0f);
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

	window().m_primlist->acquire_lock();
	uint32_t num_screens = m_chains->update_screen_textures(s_current_view, window().m_primlist->first(), window());
	window().m_primlist->release_lock();

	bool skip_frame = update_dimensions();
	if (skip_frame)
	{
		return 0;
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

	// Recreate the vector FBO if its size no longer matches the current window.
	// Avoids clipping vertices outside the FBO when the size at create() differs from the size at
	// draw() (mid window-init, user resize, etc.).
	if (window_index == 0)
	{
		const uint16_t cur_w = uint16_t(s_width[window_index]);
		const uint16_t cur_h = uint16_t(s_height[window_index]);
		const uint16_t target_fb_w = uint16_t(cur_w * m_vec_supersample);
		const uint16_t target_fb_h = uint16_t(cur_h * m_vec_supersample);
		if (cur_w > 0 && cur_h > 0 && (target_fb_w != m_vec_fb_w || target_fb_h != m_vec_fb_h))
		{
			if (bgfx::isValid(m_vec_fb))
				bgfx::destroy(m_vec_fb);
			if (bgfx::isValid(m_vec_glow_fb))
				bgfx::destroy(m_vec_glow_fb);
			m_vec_fb_w = target_fb_w;
			m_vec_fb_h = target_fb_h;
			// bilinear (no MSAA, for sampler compatibility)
			const uint64_t cf = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
				BGFX_TEXTURE_RT;
			bgfx::TextureHandle tc = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, cf);
			bgfx::TextureHandle td = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::D32F, cf);
			bgfx::TextureHandle at[2] = { tc, td };
			m_vec_fb = bgfx::createFrameBuffer(2, at, true);
			bgfx::TextureHandle gc = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::RG11B10F, cf);
			m_vec_glow_fb = bgfx::createFrameBuffer(1, &gc, true);
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
		// Advance the beam time window when the vector device started a new emulated frame.
		// The machine-time basis makes pause and fast-forward transparent; time can regress on
		// state load / rewind, in which case the window restarts from zero.
		const double vec_now = window().machine().time().as_double();
		m_vec_frame_advanced = m_vec_new_frame;
		if (m_vec_new_frame)
		{
			m_vec_win_t0 = (vec_now < m_vec_win_t1) ? 0.0 : m_vec_win_t1;
			m_vec_win_t1 = vec_now;
			m_vec_new_frame = false;
		}
		// While paused no new events arrive; keep showing the queued primitives as a still image.
		const bool vec_draw_all = window().machine().paused();
		// Window-boundary blend: treat each event as a pulse of this width and give a boundary
		// line the overlap fraction in each adjacent window (energy-conserving). This hides the
		// temporal-aliasing blink when the list period beats against the refresh (a line would
		// otherwise hop between "0 times in this window" and "twice in the next"). 0 = hard cut.
		const double vec_blend = std::max(0.0, double(m_chains->slider_value(0, "vector_window_blend", 0.0f)) * 0.001);
		auto vec_window_weight = [this, vec_draw_all, vec_blend] (const render_primitive &p) -> float
		{
			if (!m_vec_beam_events || p.t0 < 0.0 || vec_draw_all)
				return 1.0f;
			if (vec_blend <= 0.0)
				return ((p.t0 > m_vec_win_t0) && (p.t0 <= m_vec_win_t1)) ? 1.0f : 0.0f;
			const double s = std::max(p.t0, m_vec_win_t0);
			const double e = std::min(p.t0 + vec_blend, m_vec_win_t1);
			return (e > s) ? float((e - s) / vec_blend) : 0.0f;
		};

		int vector_count = 0;   // all vector lines (decides the FBO path)
		int visible_count = 0;  // lines with nonzero energy in the current window
		bool any_timed = false;
		render_primitive *scan = window().m_primlist->first();
		while (scan != nullptr)
		{
			if (scan->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(scan->flags))
			{
				vector_count++;
				if (scan->t0 >= 0.0)
					any_timed = true;
				if (vec_window_weight(*scan) > 0.0f)
					visible_count++;
			}
			scan = scan->next();
		}

		if (vector_count > 0)
		{
			// CRT flicker: dim the whole frame when the beam list was not refreshed this frame.
			// Renderer-side (bgfx only); the amount comes from the chain's vector_crt_flicker slider.
			// Untimed beam sources (and beam-event mode off) only: timed lists flicker physically
			// via the time-window assignment.
			m_crt_flicker_factor = ((!any_timed || !m_vec_beam_events) && m_vector_device != nullptr && m_vector_device->beam_list_stale())
				? std::max(0.0f, 1.0f - m_chains->slider_value(0, "vector_crt_flicker", 0.0f))
				: 1.0f;

			// Analog integrator drift (AVG, master plan 3-4): op-amp offset slowly translates the
			// whole image and the CNTR command pulls it back. Modelled per axis as a sum of two
			// incommensurate sub-Hz sines (mean zero, so it self-recentres without a sawtooth reset),
			// amplitude in 1920-reference pixels scaled to the actual resolution. analog_drift 0 = off.
			const float drift_amt = m_chains->slider_value(0, "analog_drift", 0.0f);
			if (drift_amt > 0.0f)
			{
				const float spd = std::max(0.05f, m_chains->slider_value(0, "analog_drift_speed", 1.0f));
				const float t   = float(vec_now) * spd;
				const float amp = drift_amt * (float(s_width[window().index()]) / 1920.0f);
				m_vec_drift_x = amp * (0.6f * sinf(t * 1.70f)        + 0.4f * sinf(t * 0.91f + 1.3f));
				m_vec_drift_y = amp * (0.6f * sinf(t * 1.43f + 2.1f) + 0.4f * sinf(t * 0.64f + 0.7f));
			}
			else
			{
				m_vec_drift_x = 0.0f;
				m_vec_drift_y = 0.0f;
			}

			// HV supply droop load (master plan 3-4 / 6.2): peak-track this frame's total beam energy
			// with gentle decay (so it does not flicker against vsync when a beam-event frame is stale),
			// then normalise by hv_droop_ref to a 0..1 load that put_analytic_line turns into a global
			// dim + defocus. Computed before the draw so this present's lines see the current load.
			if (m_vec_frame_advanced)
				m_hv_smoothed = std::max(m_hv_energy, m_hv_smoothed * 0.82f);
			const float hv_ref = std::max(0.01f, m_chains->slider_value(0, "hv_droop_ref", 10.0f));
			m_hv_load_norm = std::clamp(m_hv_smoothed / hv_ref, 0.0f, 1.0f);

			// Vertex-dwell endpoint dots (master plan 2-3): neighbour-aware pass over the vector list in
			// draw order. A point shared by two consecutive segments is a vertex where the beam dwells in
			// proportion to how sharply it turns (straight joint -> no dwell, sharp corner / reversal ->
			// full dwell); an unshared point is a stroke terminus where the beam stops (full dwell). The
			// per-endpoint factor scales the end-cap dot in put_analytic_line. vertex_dwell 0 = off
			// (uniform caps, the old behaviour). Only meaningful for the analytic path (it draws caps).
			const float vertex_dwell = m_line_analytic ? m_chains->slider_value(0, "vertex_dwell", 0.0f) : 0.0f;
			std::unordered_map<const render_primitive*, std::pair<float, float>> vtx_boost;
			if (vertex_dwell > 0.0f)
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
					if (pv != nullptr)
					{
						const float gx = p->bounds.x0 - pv->bounds.x1;
						const float gy = p->bounds.y0 - pv->bounds.y1;
						if (gx * gx + gy * gy < 0.25f)  // shared vertex (within 0.5 px)
						{
							const float prof = (1.0f - (pdx * ndx + pdy * ndy)) * 0.5f;  // 0 straight, 1 reversal
							vtx_boost[pv].second = prof;  // prev segment's end
							vtx_boost[p].first   = prof;  // this segment's start
						}
					}
					pv = p; pdx = ndx; pdy = ndy;
				}
			}

			// Deflection-amplifier dynamics (master plan 3-3): when on, each analytic line is drawn as a
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
			m_glow_on = m_line_analytic && bgfx::isValid(m_vec_glow_fb) && (m_chains->slider_value(0, "analytic_glow", 0.0f) > 0.0f);

			// fill vertex data (classic: quad + rounded fans; analytic: one expanded quad)
			int vertices = 0;
			// analytic core: body quad 6 + two end-cap dots 6+6 (or DEFL_NOUT*6 body + 12 caps with deflection)
			const uint32_t verts_per_line = m_line_analytic ? (m_defl_on ? uint32_t(DEFL_NOUT * 6 + 12) : 18u) : uint32_t(LINE_VERTICES_PER_LINE);
			m_vec_vpl = verts_per_line;
			const bgfx::VertexLayout &line_decl = m_line_analytic ? AnalyticLineVertex::ms_decl : ScreenVertex::ms_decl;
			bgfx::TransientVertexBuffer tvb = {};
			bgfx::TransientVertexBuffer glow_tvb = {};
			int glow_verts = 0;
			bool glow_alloc = false;
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
						if (m_glow_on)
						{
							const uint32_t gneeded = uint32_t(visible_count) * 6u;
							if (gneeded == bgfx::getAvailTransientVertexBuffer(gneeded, line_decl))
							{
								bgfx::allocTransientVertexBuffer(&glow_tvb, gneeded, line_decl);
								glow_alloc = (glow_tvb.data != nullptr);
							}
						}
						render_primitive *vprim = window().m_primlist->first();
						while (vprim != nullptr)
						{
							// Write only LINEs with PRIMFLAG_VECTOR that fall in the beam time window.
							// UI lines are drawn normally by buffer_primitives (to avoid phosphor ghosting).
							if (vprim->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(vprim->flags))
							{
								const float w = vec_window_weight(*vprim);
								if (w > 0.0f)
								{
									m_vec_line_weight = w;
									if (m_line_analytic)
									{
										float scap = 1.0f, ecap = 1.0f;
										if (vertex_dwell > 0.0f)
										{
											auto it = vtx_boost.find(vprim);
											if (it != vtx_boost.end())
											{
												scap = 1.0f + vertex_dwell * (it->second.first  - 1.0f);
												ecap = 1.0f + vertex_dwell * (it->second.second - 1.0f);
											}
										}
										AnalyticLineVertex *gptr = glow_alloc ? reinterpret_cast<AnalyticLineVertex*>(glow_tvb.data) + glow_verts : nullptr;
										put_analytic_line(vprim, reinterpret_cast<AnalyticLineVertex*>(tvb.data) + vertices, gptr, scap, ecap);
										if (gptr) glow_verts += 6;
									}
									else
										put_solid_line(vprim, reinterpret_cast<ScreenVertex*>(tvb.data) + vertices);
									vertices += verts_per_line;
								}
							}
							vprim = vprim->next();
						}
						m_vec_line_weight = 1.0f;
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
				// u_line_params.x = Overload Softness (higher = the beam defocuses sooner as it overloads)
				bgfx_uniform* lp = line_eff->uniform("u_line_params");
				if (lp)
				{
					float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f), 0.0f, 0.0f, 0.0f };
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

			// Analytic glow (案A): draw the separate glow buffer into m_vec_glow_fb (cleared, additive),
			// then inject it as "glow0" so a chain pass can add it after the shadow mask. Only when glow
			// is active and its buffer was allocated; otherwise the chain's glow pass is disablewhen-off.
			if (glow_alloc && glow_verts > 0 && bgfx::isValid(m_vec_glow_fb))
			{
				const uint16_t glow_view = uint16_t(s_current_view);
				s_current_view++;
				bgfx::setViewFrameBuffer(glow_view, m_vec_glow_fb);
				bgfx::setViewRect(glow_view, 0, 0, m_vec_fb_w, m_vec_fb_h);
				bgfx::setViewClear(glow_view, BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
				bgfx::setViewMode(glow_view, bgfx::ViewMode::Sequential);
				float gproj[16];
				bx::mtxOrtho(gproj, 0.0f, float(s_width[window_index]), float(s_height[window_index]),
					0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
				bgfx::setViewTransform(glow_view, nullptr, gproj);
				bgfx::setVertexBuffer(0, &glow_tvb);
				bgfx_effect* line_eff = (m_line_effect != nullptr) ? m_line_effect : m_gui_effect[BLENDMODE_ADD];
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
					float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f), 0.0f, 0.0f, 0.0f };
					lp->set(vals, sizeof(float) * 4);
					lp->upload();
				}
				line_eff->submit(glow_view);
			}
		}
	}

	// chain: inject the FBO into the chain system and render it.
	// Pass m_vec_fb's color attachment (attachment 0) to the chain as "screen0".
	// The chain does a passthrough blit and writes directly to the backbuffer.
	if (m_vectors_in_fbo && window_index == 0)
	{
		bgfx::TextureHandle vec_color = bgfx::getTexture(m_vec_fb, 0);
		if (bgfx::isValid(vec_color))
		{
			m_chains->inject_vector_screen(vec_color,
				uint16_t(s_width[window_index]),
				uint16_t(s_height[window_index]),
				m_vec_fb_w, m_vec_fb_h);
			// Expose the analytic-glow FBO as "glow0" for the chain's post-mask glow composite pass.
			if (bgfx::isValid(m_vec_glow_fb))
			{
				bgfx::TextureHandle glow_color = bgfx::getTexture(m_vec_glow_fb, 0);
				if (bgfx::isValid(glow_color))
					m_chains->inject_vector_glow(glow_color, m_vec_fb_w, m_vec_fb_h);
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
				// Feed the monitor-glow accumulation into the chain's glow pass (no-op without an
				// "add_mglow" pass). In beam-event mode the per-frame energy drops to 0 on frames
				// where the beam list was not refreshed (its timed points were already consumed),
				// which made the glow flicker against vsync. The monitor glow physically persists,
				// so track the peak and decay it gently instead of using the raw per-frame value.
				if (m_vec_frame_advanced)
					m_mglow_smoothed = std::max(m_mglow_amount, m_mglow_smoothed * 0.80f);
				const float mglow_vals[4] = { m_mglow_smoothed, 0.0f, 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "add_mglow", "u_mglow_amount", mglow_vals, 4);

				// Freeze the phosphor-tail pool on presents that did not advance emulation
				// (pause, menu stills): no decay, no injection. No-op without a "tail_accum" pass.
				const float tail_freeze[4] = { m_vec_frame_advanced ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "tail_accum", "u_tail_freeze", tail_freeze, 4);

				// Bloom dark-area noise: strength from the slider, and freeze the pattern (y=1) on
				// presents that did not advance emulation so the shimmer stops while paused (F5).
				const float bloom_noise[4] = {
					m_chains->slider_value(0, "bloom_noise", 0.10f),
					m_vec_frame_advanced ? 0.0f : 1.0f, 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "Bloom Apply", "u_bloom_noise", bloom_noise, 4);

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
			const float paper_white = float(m_module().options().bgfx_hdr_paper_white());

			// Seed pass: hdr_work = screen_hdr * beam_peak (linear nits). A dedicated view before
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
				if (p) { float val[4] = { beam_peak, 0,0,0 }; p->set(val, sizeof(float)*4); p->upload(); }
				bgfx_uniform *st = m_hdr_screen_effect->uniform("s_tex");
				if (st) bgfx::setTexture(0, st->handle(), screen_hdr->texture());
				bgfx::setVertexBuffer(0, &vb);
				m_hdr_screen_effect->submit(seed_view);
			}

			// Set the per-frame nits scale on the HDR gui effects (multiply stays a unit ratio).
			for (int b = 0; b < 4; b++)
			{
				if (m_hdr_gui_effect[b] == nullptr) continue;
				bgfx_uniform *u = m_hdr_gui_effect[b]->uniform("u_hdr_gui");
				if (u) { float val[4] = { (b == BLENDMODE_RGB_MULTIPLY) ? 1.0f : paper_white, 0,0,0 }; u->set(val, sizeof(float)*4); u->upload(); }
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
			// window 0 renders to the default backbuffer (m_framebuffer is null there)
			bgfx::FrameBufferHandle present_fb = BGFX_INVALID_HANDLE;
			if (m_framebuffer != nullptr)
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
					float(m_module().options().bgfx_hdr_paper_white()),
					s_bgfx_hdr_active ? 1.0f : 0.0f,
					0.0f };
				hp->set(vals, sizeof(float) * 4);
				hp->upload();
			}
			// Phosphor gamut blend (master plan 2-6): push the Rec.709 primaries toward the P22
			// phosphor chromaticities in the Rec.2020 container. 0 = off (exact 709 -> 2020).
			bgfx_uniform *pg = m_hdr_present_effect->uniform("u_phosphor_gamut");
			if (pg)
			{
				float pgv[4] = { m_chains->slider_value(0, "phosphor_gamut", 0.0f), 0.0f, 0.0f, 0.0f };
				pg->set(pgv, sizeof(float) * 4);
				pg->upload();
			}
			bgfx_uniform *st = m_hdr_present_effect->uniform("s_tex");
			if (st) bgfx::setTexture(0, st->handle(), m_hdr_work->texture());
			bgfx::setVertexBuffer(0, &vb);
			m_hdr_present_effect->submit(present_view);
		}
	}

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
		bgfx::frame();
	}


	return 0;
}

void renderer_bgfx::update_recording()
{
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
					put_packed_quad(*prim, WHITE_HASH, (ScreenVertex*)buffer->data + vertices);
					vertices += 6;
				}
				else
				{
					const uint32_t hash = get_texture_hash(*prim);
					if (atlas_valid && (*prim)->packable(PACKABLE_SIZE) && hash != 0 && m_hash_to_entry[hash].hash())
					{
						setup_ortho_view();
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
