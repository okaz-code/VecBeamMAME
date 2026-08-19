// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  slider.h - BGFX shader parameter slider
//
//============================================================

#ifndef MAME_RENDER_BGFX_SLIDER_H
#define MAME_RENDER_BGFX_SLIDER_H

#pragma once

#include <bgfx/bgfx.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct slider_state;

class running_machine;

class bgfx_slider
{
public:
	enum slider_type
	{
		SLIDER_INT_ENUM,
		SLIDER_FLOAT,
		SLIDER_INT,
		SLIDER_COLOR,
		SLIDER_VEC2
	};

	enum screen_type
	{
		SLIDER_SCREEN_TYPE_NONE = 0,
		SLIDER_SCREEN_TYPE_RASTER = 1,
		SLIDER_SCREEN_TYPE_VECTOR = 2,
		SLIDER_SCREEN_TYPE_VECTOR_OR_RASTER = SLIDER_SCREEN_TYPE_VECTOR | SLIDER_SCREEN_TYPE_RASTER,
		SLIDER_SCREEN_TYPE_LCD = 4,
		SLIDER_SCREEN_TYPE_LCD_OR_RASTER = SLIDER_SCREEN_TYPE_LCD | SLIDER_SCREEN_TYPE_RASTER,
		SLIDER_SCREEN_TYPE_LCD_OR_VECTOR = SLIDER_SCREEN_TYPE_LCD | SLIDER_SCREEN_TYPE_VECTOR,
		SLIDER_SCREEN_TYPE_ANY = SLIDER_SCREEN_TYPE_RASTER | SLIDER_SCREEN_TYPE_VECTOR | SLIDER_SCREEN_TYPE_LCD
	};

	bgfx_slider(running_machine& machine, std::string &&name, float min, float def, float max, float step, slider_type type, screen_type screen, std::string format, std::string description, std::vector<std::string>& strings);
	virtual ~bgfx_slider();

	// A MACRO slider drives other sliders instead of a uniform: one user-facing control that imports
	// derived values into the detail sliders it lists (see chain_manager::apply_macros). Declared in
	// the chain JSON with a "targets" array, so the mapping stays data-driven per chain - the same
	// "Bloom Strength" means different sliders on the colour and Vectrex chains.
	//   scale : target = target's JSON default * macro value
	//   curve : target = piecewise-linear interpolation of the declared [macro, target] points
	//   enable: target = macro > 0.5 ? target's JSON default : 0   (group on/off)
	enum macro_mode { MACRO_SCALE, MACRO_CURVE, MACRO_ENABLE };
	struct macro_target
	{
		std::string name;              // slider name including the component suffix, or the bare name
		bool        all_components = false;   // true = apply to name0 and any name1 / name2 as well
		macro_mode  mode = MACRO_SCALE;
		std::vector<float> xs, ys;     // MACRO_CURVE only, ascending in xs
	};

	int32_t update(std::string *str, int32_t newval);

	// Getters
	const std::string &name() const { return m_name; }
	slider_type type() const { return m_type; }
	float value() const { return m_value; }
	float uniform_value() const { return float(m_value); }
	float min_value() const { return m_min; }
	float default_value() const { return m_default; }
	float max_value() const { return m_max; }
	float step_value() const { return m_step; }
	slider_state *core_slider() const { return m_slider_state.get(); }
	// "advanced": true in the chain JSON. Such a slider still works and still saves/restores - it is
	// only hidden from the slider MENU while the chain's Advanced toggle is off (see
	// chain_manager::get_slider_list). Used for the "[-] " parameters that are inert at the chain
	// defaults, so the menu opens on the controls that actually do something.
	bool advanced() const { return m_advanced; }
	const std::vector<macro_target> &macro_targets() const { return m_macro_targets; }
	bool is_macro() const { return !m_macro_targets.empty(); }
	size_t size() const { return get_size_for_type(m_type); }
	static size_t get_size_for_type(slider_type type);

	// Setters
	void import(float val);
	void set_advanced(bool advanced) { m_advanced = advanced; }
	void set_macro_targets(std::vector<macro_target> &&targets) { m_macro_targets = std::move(targets); }

protected:
	std::unique_ptr<slider_state> create_core_slider();
	int32_t as_int() const { return int32_t(floor(m_value / m_step + 0.5f)); }

	std::string     m_name;
	bool            m_advanced = false;
	std::vector<macro_target> m_macro_targets;
	float           m_min;
	float           m_default;
	float           m_max;
	float           m_step;
	slider_type     m_type;
	screen_type     m_screen_type;
	std::string     m_format;
	std::string     m_description;
	std::vector<std::string> m_strings;
	float           m_value;
	std::unique_ptr<slider_state> m_slider_state;
	running_machine&m_machine;
};

#endif // MAME_RENDER_BGFX_SLIDER_H
