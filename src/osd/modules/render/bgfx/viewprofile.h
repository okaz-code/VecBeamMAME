// license:BSD-3-Clause
// copyright-holders:okaz-code
//============================================================
//
//  viewprofile.h - per-view GPU profiling labels for BGFX
//
//============================================================
//
//  bgfx fills bgfx::Stats::viewStats with one entry per view it actually
//  submitted, carrying that view's GPU begin/end timestamps.  Two conditions
//  gate it: BGFX_DEBUG_PROFILER has to be set (see renderer.h's Profiler), and
//  the backend has to support timer queries.  Each entry is labelled with
//  whatever bgfx::setViewName() last stored for the view id, so without names
//  the breakdown is a list of bare numbers.
//
//  View ids here are handed out sequentially by whoever draws next, so which
//  pass owns id 7 depends on which optional passes ran this frame.  A label
//  therefore has to be (re)applied whenever a slot changes hands, not once at
//  startup.
//
//  bgfx::setViewName() takes the resource API lock and writes into the command
//  buffer, so this is off unless -bgfx_debug asked for it, and a slot whose
//  label already matches is skipped.  In a steady state that leaves one string
//  compare per view per frame and no bgfx calls at all.
//
#ifndef MAME_RENDER_BGFX_VIEWPROFILE_H
#define MAME_RENDER_BGFX_VIEWPROFILE_H

#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bgfx_view_profile
{
	namespace detail
	{
		inline bool s_enabled = false;
		inline std::vector<std::string> s_names;
	}

	// Turned on from the renderer when bgfx_debug is set, before any view is named.
	inline void set_enabled(bool enable)
	{
		detail::s_enabled = enable;
		if (!enable)
		{
			detail::s_names.clear();
			detail::s_names.shrink_to_fit();
		}
	}

	inline bool enabled()
	{
		return detail::s_enabled;
	}

	// Whether bgfx::Stats::viewStats carries usable per-view GPU times on the active backend.
	//
	// renderer.h's Profiler reads m_gpuTimer.m_result[viewStats.view], so a backend has to both
	// implement TimerQuery::begin/end per view AND size m_result by view id. D3D9/11/12 and
	// Vulkan do both (Result m_result[BGFX_CONFIG_MAX_VIEWS+1]).
	//
	// Metal does neither: renderer_mtl.mm's TimerQueryMtl::begin()/end() are empty stubs, and
	// m_result is an 8-entry ring of WHOLE-FRAME timestamps indexed by a ring counter. Views 0-7
	// would therefore report the frame time under a pass's name, and any view past 7 - this
	// renderer uses around 35 - reads past the end of the array. So the profiler stays off there,
	// which also keeps that out-of-bounds read from ever happening.
	//
	// bgfx force-disables its own GL profiler on macOS as well (renderer_gl.cpp), so OpenGL is
	// only trusted off-Apple.
	inline bool backend_supports_view_timing()
	{
		switch (bgfx::getRendererType())
		{
		case bgfx::RendererType::Direct3D9:
		case bgfx::RendererType::Direct3D11:
		case bgfx::RendererType::Direct3D12:
		case bgfx::RendererType::Vulkan:
			return true;
		case bgfx::RendererType::OpenGL:
		case bgfx::RendererType::OpenGLES:
#if defined(__APPLE__)
			return false;
#else
			return true;
#endif
		default:
			return false;
		}
	}

	// Label a view for bgfx::Stats::viewStats and for graphics debuggers.
	inline void name(uint32_t view, const char *label)
	{
		if (!detail::s_enabled || label == nullptr)
			return;
		if (view >= detail::s_names.size())
			detail::s_names.resize(view + 16);
		std::string &cached = detail::s_names[view];
		if (cached == label)
			return;
		cached = label;
		bgfx::setViewName(bgfx::ViewId(view), label);
	}
}

#endif // MAME_RENDER_BGFX_VIEWPROFILE_H
