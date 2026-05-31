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

// slider_state 完全型 (core->description アクセス用)
#include "../frontend/mame/ui/slider.h"

// vector_options 用 (STAR WARS パラメータの static 更新)
#include "video/vector.h"

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

	bgfx::reset(wdim.width(), wdim.height(), video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);

	// Enable debug text if requested
	bool bgfx_debug = m_options->bgfx_debug();
	bgfx::setDebug(bgfx_debug ? BGFX_DEBUG_STATS : BGFX_DEBUG_TEXT);

	// Get actual maximum texture size
	bgfx::Caps const *const caps = bgfx::getCaps();
	m_max_texture_size = caps->limits.maxTextureSize;

	ScreenVertex::init();

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

	// ベクター描画用 FBO を解放
	if (bgfx::isValid(m_vec_fb))
	{
		bgfx::destroy(m_vec_fb);
		m_vec_fb = BGFX_INVALID_HANDLE;
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

	// ベクター描画用 FBO を作成（window 0 のみ）
	// chain_manager / target_manager を経由せず純粋にBGFXのAPIで管理する。
	// 色＋深度の2アタッチメントにする（BGFX は描画時に深度アタッチメントが
	// あることを前提とするケースがある）。bgfx_target.cpp の作り方を踏襲。
	if (window().index() == 0)
	{
		m_vec_fb_w = uint16_t(wdim.width() * VEC_SUPERSAMPLE);
		m_vec_fb_h = uint16_t(wdim.height() * VEC_SUPERSAMPLE);
		// 後の draw() で size 不一致時に再作成するため、createFrameBuffer ヘルパー化
		// （ここでは初期作成のみ。draw() の再作成ロジックでも同等のコードを使う）
		// POINT フィルタフラグ削除 → デフォルト bilinear。
		// MSAA は標準 sampler でサンプル不可なので使わず、supersample + fs_sw_line で対応。
		const uint64_t color_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
			BGFX_TEXTURE_RT;
		const uint64_t depth_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
			BGFX_TEXTURE_RT;
		bgfx::TextureHandle tex_color = bgfx::createTexture2D(
			m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::BGRA8, color_flags);
		bgfx::TextureHandle tex_depth = bgfx::createTexture2D(
			m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::D32F, depth_flags);
		bgfx::TextureHandle attachments[2] = { tex_color, tex_depth };
		m_vec_fb = bgfx::createFrameBuffer(2, attachments, true);  // true=textures owned by FBO

	}

	// 解析的 AA ベクター線エフェクト読み込み
	if (window().index() == 0)
	{
		m_line_effect = m_effects->get_or_load_effect(m_module().options(), "sw_line");

		// 全 25 UI slider は vector-starwars.json の sliders セクションに
		// 移動済み。chain system が UI 登録 + config 永続化を担当する。
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

	bgfx_effect** effects = is_screen ? m_screen_effect : m_gui_effect;

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

// 解析的 AA フェード対応ベクター線描画
// 6 頂点で線を 1 つのクワッドとして描画。頂点 UV は線幅方向に 0..1 で展開し、
// fs_sw_line.sc 内で `1 - (2|v-0.5|)^2` のパラボラフェードを計算する。
// → GPU 補間が線形なので、角度に関係なく同じ線幅プロファイルが得られる。
// 角丸線端 — 各端に N 個の三角形ファンを追加（半円近似）
// 6 (線本体) + 2 × N × 3 = 6 + 6N 頂点/線。N=8 で 54 頂点。
//   line_solid_vertices_per_line() で参照される。
static constexpr int LINE_CAP_SEGMENTS = 8;
static constexpr int LINE_VERTICES_PER_LINE = 6 + 2 * LINE_CAP_SEGMENTS * 3;

void renderer_bgfx::put_solid_line(render_primitive *prim, ScreenVertex* vertex)
{
	float width = prim->width < 0.5f ? 0.5f : prim->width;
	float x0 = prim->bounds.x0;
	float y0 = prim->bounds.y0;
	float x1 = prim->bounds.x1;
	float y1 = prim->bounds.y1;

	// MAME HLSL vector.fx の length-based fade を移植
	// (vector_length_scale / vector_length_ratio)。長線ほどビーム輝度が落ちる
	// CRT 電子ビーム電流負荷を再現。HLSL は GPU で各ピクセル後に乗算するが、
	// 本 mod は CPU 側 sw_line で頂点色に組み込む。式は HLSL と同等:
	//   norm_len     = pixel_len / max(window_w, window_h)
	//   lengthModulate = 1 - clamp(norm_len / ratio, 0, 1)
	//   timeLengthModulate = lerp(1, lengthModulate, scale)
	const float vls_scale = m_chains->slider_value(0, "vector_length_scale", 0.0f);
	const float vls_ratio = m_chains->slider_value(0, "vector_length_ratio", 0.5f);
	float length_factor = 1.0f;
	if (vls_scale > 0.0001f && vls_ratio > 0.0001f)
	{
		const float pix_dx = x1 - x0;
		const float pix_dy = y1 - y0;
		const float pix_len = sqrtf(pix_dx * pix_dx + pix_dy * pix_dy);
		const float screen_ref = float(std::max(s_width[window().index()], s_height[window().index()]));
		const float norm_len = (screen_ref > 0.0f) ? (pix_len / screen_ref) : 0.0f;
		const float length_modulate = 1.0f - std::min(norm_len / vls_ratio, 1.0f);
		// lerp(1, length_modulate, scale)
		length_factor = 1.0f + vls_scale * (length_modulate - 1.0f);
	}

	// RGB クロスチャネル加算は post-process 化 (sw_color_mix shader)。
	// ここでは線色をそのままパック。同色重なりでの二重加算問題が解消。
	uint32_t rgba = u32Color(
		uint32_t(prim->color.r * length_factor * 255.0f + 0.5f),
		uint32_t(prim->color.g * length_factor * 255.0f + 0.5f),
		uint32_t(prim->color.b * length_factor * 255.0f + 0.5f),
		uint32_t(prim->color.a * 255.0f + 0.5f));

	// Overload defocus
	// prim->overload は vector.cpp の STARWARS パスで sigmoid から算出済み (0..1)
	// 太さ膨張は vector.cpp::screen_update で beam_width (= prim->width) に既に反映済み
	// (STARWARS Overload Max Width slider で制御)。put_solid_line では追加の倍率を掛けない。
	// m_u に overload を格納し、fs_sw_line で fade 関数 (parabola↔Gaussian) を補間。
	const float ovld = (prim->overload < 0.0f) ? 0.0f :
	                   (prim->overload > 1.0f) ? 1.0f : prim->overload;

	float dx = x1 - x0;
	float dy = y1 - y0;
	const float seg_len = sqrtf(dx * dx + dy * dy);
	// 点扱い判定 (seg_len <= threshold で body skip + 単一円)
	// MAME の add_point は x0=x1, y0=y1 で seg_len=0 になる。
	// 通常モードで描くと 2 つの半円 cap が同じ中心に重なって輝度 2 倍 + 形状歪み。
	const bool as_point = (seg_len <= m_chains->slider_value(0, "line_point_threshold", 2.0f));
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

	// 線方向に直交する法線（正規化済み）
	float nx = dy;
	float ny = -dx;

	// AA フェード分を含むためクワッド幅を 1 ピクセル分余裕を持たせる
	float r = width * 0.5f + 0.5f;
	if (r < 1.0f) r = 1.0f;

	// 線本体は端を延長せず (x0..x1) のクワッドにする。
	// 端の延長は角丸ファンが担う。これによりキャップと線本体の境界が連続する。

	// 線本体クワッド 4 頂点（インデックス: 0=start+n, 1=end+n, 2=end-n, 3=start-n）
	const float qx[4] = { x0 + nx * r, x1 + nx * r, x1 - nx * r, x0 - nx * r };
	const float qy[4] = { y0 + ny * r, y1 + ny * r, y1 - ny * r, y0 - ny * r };
	const float qv[4] = { 0.0f,        0.0f,        1.0f,        1.0f        };

	// cap 用のブースト色 (中央頂点だけ明るくする)
	// line_cap_brightness slider で RGB を 1..3 倍にスケール、255 で clamp。
	const float cap_bright = std::max(1.0f, m_chains->slider_value(0, "line_cap_brightness", 1.0f));
	uint32_t cap_center_rgba = rgba;
	if (cap_bright > 1.0001f)
	{
		const uint32_t r8 = std::min<uint32_t>(uint32_t(prim->color.r * length_factor * cap_bright * 255.0f + 0.5f), 255);
		const uint32_t g8 = std::min<uint32_t>(uint32_t(prim->color.g * length_factor * cap_bright * 255.0f + 0.5f), 255);
		const uint32_t b8 = std::min<uint32_t>(uint32_t(prim->color.b * length_factor * cap_bright * 255.0f + 0.5f), 255);
		const uint32_t a8 = std::min<uint32_t>(uint32_t(prim->color.a * 255.0f + 0.5f), 255);
		cap_center_rgba = u32Color(r8, g8, b8, a8);
	}

	auto setv = [&](int i, float x, float y, float v_value, uint32_t vrgba = 0) {
		vertex[i].m_x = x;
		vertex[i].m_y = y;
		vertex[i].m_z = 0.0f;
		vertex[i].m_rgba = (vrgba == 0) ? rgba : vrgba;
		vertex[i].m_u = ovld;     // 線単位の overload（fs_sw_line で fade 関数を補間）
		vertex[i].m_v = v_value;  // 線幅方向 [0..1]、0.5=中央
	};

	// line_cap_size はピクセル半径として解釈。
	// 線本体半径 r より cap が大きい時のみ膨張。線が太い時は cap 拡張なし (= 角丸線)。
	// 1920×1080 基準で解像度に比例スケール。これで window 解像度を
	// 変えても画面上の cap 相対サイズが一定になる。
	const float cap_ref_width  = 1920.0f;
	const float cap_res_scale  = float(s_width[window().index()]) / cap_ref_width;
	// cap サイズを線 intensity (= prim->color.a) に連動。
	//   cap_px = cap_min + (cap_full - cap_min) * pow(intensity, curve)
	//   curve=0 (default) で pow=1 → cap = cap_full = 従来挙動。
	//   curve>0 で暗い線ほど cap が line_cap_min_size に向かって縮小。
	const float cap_full   = m_chains->slider_value(0, "line_cap_size", 4.0f) * cap_res_scale;
	const float cap_min_px = m_chains->slider_value(0, "line_cap_min_size", 0.0f) * cap_res_scale;
	const float cap_curve  = m_chains->slider_value(0, "line_cap_intensity_curve", 0.0f);
	const float cap_bi     = std::clamp(prim->color.a, 0.0f, 1.0f);
	const float cap_f      = (cap_curve <= 0.0001f) ? 1.0f : powf(cap_bi, cap_curve);
	const float fixed_cap_radius = std::max(0.0f, cap_min_px + (cap_full - cap_min_px) * cap_f);
	const float cap_extent       = std::max(0.0f, fixed_cap_radius - r);

	int vi = 6;

	if (as_point)
	{
		// 点扱い分岐
		// body をスキップ (6 頂点を中心に degenerate trianglesで塗りつぶし)、
		// 線端 2 つ分の頂点予算 (48) を単一 360° 全周ファンに使う。
		// cap が二重に重ならず、点として綺麗な円を 1 つ描画。
		const float cx = (x0 + x1) * 0.5f;
		const float cy = (y0 + y1) * 0.5f;

		// 線本体スロット 0-5: 中心点で degenerate (= zero-area、GPU が culling)
		for (int i = 0; i < 6; ++i)
			setv(i, cx, cy, 0.5f, cap_center_rgba);

		// 全周ファン: LINE_CAP_SEGMENTS * 2 = 16 セグメント、48 頂点
		const int full_segs = LINE_CAP_SEGMENTS * 2;
		const float dtheta_full = 2.0f * 3.14159265f / float(full_segs);
		const float pr = std::max(r, fixed_cap_radius);  // 点も固定半径以上を保証
		for (int i = 0; i < full_segs; ++i)
		{
			const float t0 = float(i) * dtheta_full;
			const float t1 = float(i + 1) * dtheta_full;
			const float p0x = cx + pr * cosf(t0);
			const float p0y = cy + pr * sinf(t0);
			const float p1x = cx + pr * cosf(t1);
			const float p1y = cy + pr * sinf(t1);

			setv(vi++, cx,  cy,  0.5f, cap_center_rgba);  // 中心: boost色 fade=1
			setv(vi++, p0x, p0y, 0.0f);                   // 外周 fade=0
			setv(vi++, p1x, p1y, 0.0f);
		}
	}
	else
	{
		// 線本体: 三角形 1 (0,1,2)、三角形 2 (0,2,3)
		setv(0, qx[0], qy[0], qv[0]);
		setv(1, qx[1], qy[1], qv[1]);
		setv(2, qx[2], qy[2], qv[2]);
		setv(3, qx[0], qy[0], qv[0]);
		setv(4, qx[2], qy[2], qv[2]);
		setv(5, qx[3], qy[3], qv[3]);

		// 端点角丸ファン
		// 半径 r(θ) = r + cap_extent * sin θ:
		//   θ=0   → r              (線本体コーナーと連続)
		//   θ=π/2 → r + cap_extent (= fixed_cap_radius if cap > r、else r で従来通り角丸線)
		//   θ=π   → r              (反対側コーナーと連続)
		// 細い線では cap_extent > 0 で円が膨らんで「玉」になる。
		// 太い線 (r >= fixed_cap_radius) では cap_extent=0 で単なる半円 = 角丸線。
		const float dtheta = 3.14159265f / float(LINE_CAP_SEGMENTS);

		auto add_cap = [&](float cx, float cy, float ax, float ay) {
			for (int i = 0; i < LINE_CAP_SEGMENTS; ++i)
			{
				const float t0 = float(i) * dtheta;
				const float t1 = float(i + 1) * dtheta;
				const float c0 = cosf(t0), s0 = sinf(t0);
				const float c1 = cosf(t1), s1 = sinf(t1);
				// 半径 r0/r1 = r + cap_extent * sin θ。axis 方向 (θ=π/2) でのみ cap_extent 加算。
				const float r0 = r + cap_extent * s0;
				const float r1 = r + cap_extent * s1;

				const float p0x = cx + r0 * (c0 * nx + s0 * ax);
				const float p0y = cy + r0 * (c0 * ny + s0 * ay);
				const float p1x = cx + r1 * (c1 * nx + s1 * ax);
				const float p1y = cy + r1 * (c1 * ny + s1 * ay);

				// 三角形: center (boosted color) → p0 (normal color) → p1
				setv(vi++, cx,  cy,  0.5f, cap_center_rgba);  // fade=1, boost
				setv(vi++, p0x, p0y, 0.0f);                   // fade=0, normal
				setv(vi++, p1x, p1y, 0.0f);                   // fade=0, normal
			}
		};

		add_cap(x0, y0, -dx, -dy);  // 始点側 cap（線の進行方向と逆方向に半円）
		add_cap(x1, y1,  dx,  dy);  // 終点側 cap（線の進行方向に半円）
	}
}

int renderer_bgfx::draw(int update)
{
	int window_index = window().index();

	// Monitor Glow の集計は vector.cpp::screen_update が行い vector_options::s_monitor_glow_amount
	// に書く。drawbgfx は下記の slider push と inject_entry_uniform で chain に渡すのみ。

	// STAR WARS + Monitor Glow パラメータを毎フレーム vector_options static
	// に push。slider 値は chain JSON 由来 (vector-starwars.json)。
	if (window_index == 0)
	{
		// 全 vector ゲーム共通 overload パラメータに改名
		vector_options::s_overload_sigmoid_k        = m_chains->slider_value(0, "overload_sigmoid_k",        0.0f);
		vector_options::s_overload_threshold        = m_chains->slider_value(0, "overload_threshold",        1.0f);
		vector_options::s_overload_width_curve_low  = m_chains->slider_value(0, "overload_width_curve_low",  0.0f);
		vector_options::s_overload_width_curve_high = m_chains->slider_value(0, "overload_width_curve_high", 0.0f);
		vector_options::s_overload_width_max        = m_chains->slider_value(0, "overload_width_max",        5.0f);
		vector_options::s_mglow_threshold       = m_chains->slider_value(0, "mglow_threshold",             0.5f);
		vector_options::s_mglow_min_distance    = m_chains->slider_value(0, "mglow_min_distance",          0.10f);
		vector_options::s_mglow_coefficient     = m_chains->slider_value(0, "mglow_coefficient",           1.0f);
		// ベクター CRT フリッカー再現 (chain slider → vector_options static)
		vector_options::s_vector_flicker        = m_chains->slider_value(0, "vector_flicker",              0.0f);
		// chain JSON に beam_width_min/max slider があれば ini を上書き。
		// default は ini 由来値 (s_*_ini) なので、slider 不在 chain では ini 値に戻る。
		vector_options::s_beam_width_min        = m_chains->slider_value(0, "beam_width_min", vector_options::s_beam_width_min_ini);
		vector_options::s_beam_width_max        = m_chains->slider_value(0, "beam_width_max", vector_options::s_beam_width_max_ini);
		vector_options::s_beam_dot_size         = m_chains->slider_value(0, "beam_dot_size", vector_options::s_beam_dot_size_ini);
		// 点ベクター専用サイズ (beam_width 非依存、点 intensity で min..max を curve 補間)
		vector_options::s_beam_dot_size_min     = m_chains->slider_value(0, "beam_dot_size_min", 0.1f);
		vector_options::s_beam_dot_size_max     = m_chains->slider_value(0, "beam_dot_size_max", 0.5f);
		vector_options::s_beam_dot_size_curve   = m_chains->slider_value(0, "beam_dot_size_curve", 0.0f);
		// ini の flicker を chain slider "vector_fake_flicker" で上書き可能に
		// (MAME 標準のランダム輝度フリッカー。世代カウンタ式 vector_flicker とは別物)
		vector_options::s_flicker               = m_chains->slider_value(0, "vector_fake_flicker", vector_options::s_flicker_ini);
		// オーバースキャン zoom (chain slider、default 1.0 = 等倍)
		vector_options::s_overscan_x            = m_chains->slider_value(0, "overscan_x", 1.0f);
		vector_options::s_overscan_y            = m_chains->slider_value(0, "overscan_y", 1.0f);
	}

	m_seen_views.clear();
	if (m_ortho_view)
		m_ortho_view->set_index(UINT_MAX);

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

	// 全 slider は chain JSON 由来で、chain_manager 標準の load_config 経由で復元される。

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

	// UI を vector blit の上に重ねる構成。原 MAME との差分: view 0 を UI 兼クリアに使うと
	// vector blit が UI 上に ADD 合成されて UI が見えなくなる/残像化するため、view 0 は手動
	// クリア専用にし、UI 用 m_ortho_view は buffer_primitives 内で late index に確保して最後に描画する。
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

	// ベクター FBO のサイズが現在のウィンドウと不一致なら再作成
	// create() 時のサイズと draw() 時のサイズが異なる場合（ウィンドウ初期化の途中、
	// ユーザーリサイズ等）に、頂点が FBO 範囲外にクリップされる問題を回避する。
	if (window_index == 0)
	{
		const uint16_t cur_w = uint16_t(s_width[window_index]);
		const uint16_t cur_h = uint16_t(s_height[window_index]);
		const uint16_t target_fb_w = uint16_t(cur_w * VEC_SUPERSAMPLE);
		const uint16_t target_fb_h = uint16_t(cur_h * VEC_SUPERSAMPLE);
		if (cur_w > 0 && cur_h > 0 && (target_fb_w != m_vec_fb_w || target_fb_h != m_vec_fb_h))
		{
			if (bgfx::isValid(m_vec_fb))
				bgfx::destroy(m_vec_fb);
			m_vec_fb_w = target_fb_w;
			m_vec_fb_h = target_fb_h;
			// bilinear（MSAA は sampler 互換性のため不使用）
			const uint64_t cf = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
				BGFX_TEXTURE_RT;
			bgfx::TextureHandle tc = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::BGRA8, cf);
			bgfx::TextureHandle td = bgfx::createTexture2D(m_vec_fb_w, m_vec_fb_h, false, 1, bgfx::TextureFormat::D32F, cf);
			bgfx::TextureHandle at[2] = { tc, td };
			m_vec_fb = bgfx::createFrameBuffer(2, at, true);

		}
	}

	// ===== ベクター LINE を FBO に描画 =====
	// 純粋にBGFX APIで FBO へ直接描画する。chain_manager / bgfx_ortho_view は経由しない。
	// 成功すれば m_vectors_in_fbo=true をセット、buffer_primitives は該当 LINE をスキップ、
	// 直後に FBO の内容を backbuffer に加算 blit する。
	m_vectors_in_fbo = false;
	if (window_index == 0 && bgfx::isValid(m_vec_fb) && atlas_valid)
	{
		// 全 LINE primitive を FBO 対象に。
		// 一部の vector line が PRIMFLAG_VECTOR を持たないケースがあり（クリッピング由来？）、
		// そういう線が halo なしになる問題を回避するため。STAR WARS では UI も MAME UI も
		// ほぼすべて LINE 描画なので、全 LINE 統一処理で問題なし。
		// UI 線が phosphor 経路に入って残像化するのを防ぐため、
		// PRIMFLAG_VECTOR を持つ LINE のみ FBO に流す。UI / MAME メニューの LINE は通常の
		// buffer_primitives 経路 (View 0、即時消える) に残す。
		int vector_count = 0;
		render_primitive *scan = window().m_primlist->first();
		while (scan != nullptr)
		{
			if (scan->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(scan->flags))
				vector_count++;
			scan = scan->next();
		}

		if (vector_count > 0)
		{
			// 1 線 = 線本体 6 + 両端角丸 fan 2×N×3 = LINE_VERTICES_PER_LINE 頂点
			const uint32_t needed = uint32_t(vector_count) * uint32_t(LINE_VERTICES_PER_LINE);
			if (needed == bgfx::getAvailTransientVertexBuffer(needed, ScreenVertex::ms_decl))
			{
				bgfx::TransientVertexBuffer tvb;
				bgfx::allocTransientVertexBuffer(&tvb, needed, ScreenVertex::ms_decl);
				if (tvb.data)
				{
					// 頂点データを埋める（put_solid_line で AA 無しクワッド + 角丸 fan）
					int vertices = 0;
					ScreenVertex* vptr = reinterpret_cast<ScreenVertex*>(tvb.data);

					// Monitor Glow 検出は vector.cpp::screen_update
					// に移動。off-screen の unclipped coords を使って集計する。drawbgfx は
					// vector_options::s_monitor_glow_amount を読むだけ。
					// （drawbgfx 側で clip 済 primitive 端点を見ても off-screen 検出はできない）

					render_primitive *vprim = window().m_primlist->first();
					while (vprim != nullptr)
					{
						// PRIMFLAG_VECTOR 付きの LINE のみ FBO に書く。
						// UI 線は buffer_primitives で通常描画する（phosphor 残像化を防ぐ）。
						if (vprim->type == render_primitive::LINE && PRIMFLAG_GET_VECTOR(vprim->flags))
						{
							put_solid_line(vprim, vptr + vertices);
							vertices += LINE_VERTICES_PER_LINE;
						}
						vprim = vprim->next();
					}

					if (vertices > 0)
					{
						// FBO 描画用 view を確保
						const uint16_t fbo_view = uint16_t(s_current_view);
						s_current_view++;
						bgfx::setViewFrameBuffer(fbo_view, m_vec_fb);
						// viewport: FBO 全領域（スーパーサンプル後の解像度）
						bgfx::setViewRect(fbo_view, 0, 0, m_vec_fb_w, m_vec_fb_h);
						bgfx::setViewClear(fbo_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
						bgfx::setViewMode(fbo_view, bgfx::ViewMode::Sequential);

						// target bounds を 1x に戻したため primitive 座標も 1x。
						// projection は 1x window 範囲、viewport は m_vec_fb (= 2x window)。
						// 1x 座標を 2x viewport にラスタライズ → GPU 側で sub-pixel 精度向上
						// (set_bounds 由来の primitive 丸め解像度向上はなくなるが、ラスタライザ
						// 解像度 2x + fs_sw_line の解析的 AA + 25-tap halo で吸収する)。
						float proj[16];
						const bgfx::Caps* caps = bgfx::getCaps();
						bx::mtxOrtho(proj, 0.0f,
							float(s_width[window_index]),
							float(s_height[window_index]),
							0.0f, 0.0f, 100.0f, 0.0f, caps->homogeneousDepth);
						bgfx::setViewTransform(fbo_view, nullptr, proj);

						bgfx::setVertexBuffer(0, &tvb);
						// テクスチャ不要、fs_sw_line がシェーダー内でフェード計算
						bgfx_effect* line_eff = (m_line_effect != nullptr) ? m_line_effect : m_gui_effect[BLENDMODE_ADD];
						bgfx_uniform* inv = line_eff->uniform("u_inv_view_dims");
						if (inv)
						{
							float values[2] = { -1.0f / float(s_width[window_index]), 1.0f / float(s_height[window_index]) };
							inv->set(values, sizeof(float) * 2);
							inv->upload();
						}
						// u_line_params.x = Overload Softness slider
						bgfx_uniform* lp = line_eff->uniform("u_line_params");
						if (lp)
						{
							float vals[4] = { m_chains->slider_value(0, "overload_softness", 1.0f), 0.0f, 0.0f, 0.0f };
							lp->set(vals, sizeof(vals));
							lp->upload();
						}
						line_eff->submit(fbo_view);
						m_vectors_in_fbo = true;
					}
				}
			}
		}
	}

	// chain: FBO を chain system に注入して描画
	// m_vec_fb のカラーアタッチメント (attachment 0) を "screen0" として chain に渡す。
	// chain がパススルー blit を行い backbuffer に直接書き込む。
	if (m_vectors_in_fbo && window_index == 0)
	{
		bgfx::TextureHandle vec_color = bgfx::getTexture(m_vec_fb, 0);
		if (bgfx::isValid(vec_color))
		{
			m_chains->inject_vector_screen(vec_color,
				uint16_t(s_width[window_index]),
				uint16_t(s_height[window_index]),
				m_vec_fb_w, m_vec_fb_h);
			// chain がロードされた後、初回のみ config から slider 値を復元する。
			// ベクターゲームでは num_screens=0 のため、後段の `if (num_screens) { load_config }`
			// ブロックが発火しない。ここで明示的に呼ぶ必要がある。
			if (m_config && m_chains->has_applicable_chain(0))
			{
				osd_printf_verbose("BGFX: Applying configuration (vector mode) for window %d\n", window().index());
				m_chains->load_config(*m_config->get_first_child());
				m_config.reset();
			}
			// Guard against null chain (e.g. chain change from menu, or failed load).
			if (m_chains->has_applicable_chain(0))
			{
				// u_mglow_amount は CPU 集計値なので毎フレーム inject。
				// vector.cpp::screen_update が前フレーム終了時に s_monitor_glow_amount に書く。
				const float mglow_vals[4] = { vector_options::s_monitor_glow_amount, 0.0f, 0.0f, 0.0f };
				m_chains->inject_entry_uniform(0, "add_mglow", "u_mglow_amount", mglow_vals, 4);

				uint32_t chain_views = m_chains->process_screen_chains(s_current_view, window());
				s_current_view += chain_views;
			}
		}
	}


	// m_ortho_view (UI 用 view) を late index に強制確保。
	// これを呼ばないと、フレーム内の全 primitive が VECTOR/VECTORBUF で skip された場合に
	// buffer_primitives 内の lazy 確保が走らず、m_ortho_view が null のまま
	// 後段の `m_gui_effect[blend]->submit(m_ortho_view->get_index())` で null deref。
	if (window_index == 0)
		setup_ortho_view();

	render_primitive *prim = window().m_primlist->first();
	std::vector<void*> sources;
	while (prim != nullptr)
	{
		uint32_t blend = PRIMFLAG_GET_BLENDMODE(prim->flags);

		// allocate_buffer は vertices==0 の時に buffer を初期化しない。
		// 全 prim が skip 対象 (VECTOR LINE + VECTORBUF QUAD) のフレームではこの状態になり、
		// 後段の bgfx::setVertexBuffer が garbage handle 経由で null deref を起こす。
		// 明示的にゼロ初期化しておくことで「未確保」を data==nullptr で検出可能にする。
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
			// m_ortho_view 必須ガード + buffer 確保チェック。
			// 直前で setup_ortho_view 強制呼出済みだが、shutdown 等で異常状態の時に null deref を防止。
			// buffer.data == nullptr の場合は allocate_buffer が vertices==0 で何もせず帰った状態
			// (全 prim skip された)。setVertexBuffer に渡すと garbage handle で BGFX 内で crash。
			bgfx::setVertexBuffer(0, &buffer);
			bgfx::setTexture(0, m_gui_effect[blend]->uniform("s_tex")->handle(), m_texture_cache->texture());

			bgfx_uniform* inv_view_dims = m_gui_effect[blend]->uniform("u_inv_view_dims");
			if (inv_view_dims)
			{
				float values[2] = { -1.0f / s_width[window_index], 1.0f / s_height[window_index] };
				inv_view_dims->set(values, sizeof(float) * 2);
				inv_view_dims->upload();
			}

			m_gui_effect[blend]->submit(m_ortho_view->get_index());
		}

		if (status != BUFFER_DONE && status != BUFFER_PRE_FLUSH)
		{
			prim = prim->next();
		}
	}

	window().m_primlist->release_lock();

	// blit ブロックは buffer_primitives 前に移動済み（上記参照）。
	// ここでは何もしない。UI は buffer_primitives 内で m_ortho_view (late view) に submit 済み。

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
		bgfx::reset(width, height, video_config.waitvsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);
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
	if (!m_ortho_view)
	{
		m_ortho_view = std::make_unique<bgfx_ortho_view>(this, 0, m_framebuffer, m_seen_views);
		// UI view を blit 後の late index に配置する設計のため、
		// m_ortho_view 自身は backbuffer をクリアしない（既に手動 clear view で済んでいる）。
		m_ortho_view->disable_color_clear();
	}
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
		// target bounds は 1x window のまま (2x supersample bounds にすると MAME UI の glyph
		// 描画で atlas bleeding = 隣セル "|" が文字間に出現する)。vector 線の supersample は
		// vec_fb (= 2x window) のラスタライザ解像度のみで提供する。
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
				// PRIMFLAG_VECTOR 付き LINE のみ FBO 経由で描画済み、
				// それ以外 (UI / MAME メニュー) は通常パスで View 0 backbuffer に直接描画。
				// 側の選択と対称化。phosphor 残像が UI に乗らないようにする。
				if (m_vectors_in_fbo && PRIMFLAG_GET_VECTOR((*prim)->flags))
					break;
				setup_ortho_view();
				put_packed_line(*prim, (ScreenVertex*)buffer->data + vertices);
				vertices += 30;
				break;

			case render_primitive::QUAD:
				// VECTORBUF 付きの背景クワッド (vector.cpp が描く
				// 黒い背景) は vec blit を上書きするためスキップ。
				// vec blit が既に backbuffer を埋めているので不要。
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
				// buffer_primitives 側と対称化、
				// PRIMFLAG_VECTOR 付きのみ FBO 経由でスキップ。UI 線は頂点を予約。
				if (m_vectors_in_fbo && PRIMFLAG_GET_VECTOR(prim->flags))
					break;
				vertices += 30;
				break;

			case render_primitive::QUAD:
				// buffer_primitives 側のスキップと対称化
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
	// 全 25 slider は chain 由来になったため append 不要。
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
	// 全 slider は chain_manager 標準の <screen> 永続化で保存される。
}
