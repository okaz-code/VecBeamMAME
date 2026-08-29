// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  vertex.h - BGFX screen vertex data
//
//============================================================

#pragma once

#ifndef __DRAWBGFX_VERTEX__
#define __DRAWBGFX_VERTEX__

#include <bgfx/bgfx.h>

struct ScreenVertex
{
	float m_x;
	float m_y;
	float m_z;
	uint32_t m_rgba;
	float m_u;
	float m_v;

	static void init()
	{
		ms_decl.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_decl;
};

// Vertex for the analytic gaussian line renderer: per-line data (signed axial distances
// from both endpoints, perpendicular distance, sigma) rides a 4-component TEXCOORD1 so a
// single submit can carry every line's own parameters.
struct AnalyticLineVertex
{
	float m_x;
	float m_y;
	float m_z;
	uint32_t m_rgba;
	float m_u;      // flat-core half-width in pixels (0 = plain gaussian cross-section)
	float m_v;      // per-primitive Long-line classification (glow MRT metadata)
	float m_a;      // signed axial distance from p0
	float m_b;      // signed axial distance from p1 (= a - len)
	float m_d;      // perpendicular distance (line) / second axis offset (point)
	float m_sigma;  // gaussian sigma in pixels; negative flags point mode
	float m_end_start;      // start-end width profile amount (0..1)
	float m_end_finish;     // finish-end width profile amount (0..1)
	float m_end_core;       // flat-core half-width at a fully active endpoint
	float m_end_transition; // distance over which endpoint width returns to the body width
	// Terminus dwell gain: 1 = no boost. The beam sitting still while Z transitions at a stroke
	// terminus deposits energy there, which the width profile alone cannot express (it keeps the
	// body's peak brightness by design). See vertex-dwell-energy-plan.md.
	float m_end_gain_start;
	float m_end_gain_finish;

	static void init()
	{
		ms_decl.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord2, 4, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord3, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_decl;
};

#endif // __DRAWBGFX_VERTEX__
