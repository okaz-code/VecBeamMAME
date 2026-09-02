// license:BSD-3-Clause
// copyright-holders:Ryan Holtz
//============================================================
//
//  targetreader.cpp - BGFX target JSON reader
//
//============================================================

#include "targetreader.h"

#include "bgfxutil.h"
#include "chainmanager.h"
#include "target.h"

#include "modules/lib/osdobj_common.h"

#include <cmath>

const target_reader::string_to_enum target_reader::STYLE_NAMES[target_reader::STYLE_COUNT] = {
	{ "guest",  TARGET_STYLE_GUEST },
	{ "native", TARGET_STYLE_NATIVE },
	{ "custom", TARGET_STYLE_CUSTOM }
};

bgfx_target* target_reader::read_from_value(
		const Value& value,
		const std::string &prefix,
		chain_manager& chains,
		uint32_t screen_index,
		uint16_t user_prescale,
		uint16_t max_prescale_size)
{
	if (!validate_parameters(value, prefix))
	{
		return nullptr;
	}

	std::string target_name = value["name"].GetString();
	uint32_t mode = uint32_t(get_enum_from_value(value, "mode", TARGET_STYLE_NATIVE, STYLE_NAMES, STYLE_COUNT));
	bool bilinear = get_bool(value, "bilinear", true);
	bool double_buffer = get_bool(value, "doublebuffer", true);

	// Allow the target format to be specified in the chain JSON.
	// The HLSL-D3D-compatible bloom needs RGBA16F for the bloom_sum target (to avoid clamping at 1.0).
	// Defaults to BGRA8 when unspecified.
	bgfx::TextureFormat::Enum format = bgfx::TextureFormat::BGRA8;
	if (value.HasMember("format") && value["format"].IsString())
	{
		std::string fmt_str = value["format"].GetString();
		if      (fmt_str == "RGBA16F") format = bgfx::TextureFormat::RGBA16F;
		else if (fmt_str == "RGBA32F") format = bgfx::TextureFormat::RGBA32F;
		else if (fmt_str == "RGBA16")  format = bgfx::TextureFormat::RGBA16;
		else if (fmt_str == "RG11B10F") format = bgfx::TextureFormat::RG11B10F;
		else if (fmt_str == "RGBA8")   format = bgfx::TextureFormat::RGBA8;
		else if (fmt_str == "BGRA8")   format = bgfx::TextureFormat::BGRA8;
		else
		{
			READER_WARN(false, "%sTarget '%s': unknown format '%s', falling back to BGRA8\n", prefix, target_name, fmt_str);
		}
	}
	float scale = 1.0f;
	if (value.HasMember("scale"))
	{
		// fractional scales (e.g. 0.5 for half-resolution) are allowed
		scale = float(value["scale"].GetDouble());
		if (!READER_CHECK(scale > 0.0f, "%sTarget '%s': Value 'scale' must be positive\n", prefix, target_name)) return nullptr;
	}
	bool use_user_prescale = get_bool(value, "user_prescale", false);

	uint16_t width = 0;
	uint16_t height = 0;
	uint16_t xprescale = 1;
	uint16_t yprescale = 1;
	switch (mode)
	{
		case TARGET_STYLE_GUEST:
		{
			width = chains.targets().width(TARGET_STYLE_GUEST, screen_index);
			height = chains.targets().height(TARGET_STYLE_GUEST, screen_index);

			if (use_user_prescale)
			{
				xprescale = user_prescale;
				yprescale = user_prescale;
				bgfx_util::find_prescale_factor(width, height, max_prescale_size, xprescale, yprescale);
			}
			break;
		}
		case TARGET_STYLE_NATIVE:
			width = chains.targets().width(TARGET_STYLE_NATIVE, screen_index);
			height = chains.targets().height(TARGET_STYLE_NATIVE, screen_index);

			if (use_user_prescale)
			{
				xprescale = user_prescale;
				yprescale = user_prescale;
				bgfx_util::find_prescale_factor(width, height, max_prescale_size, xprescale, yprescale);
			}
			break;
		case TARGET_STYLE_CUSTOM:
			READER_WARN(!value.HasMember("user_prescale"), "%sTarget '%s': user_prescale parameter is not used for custom-type render targets.\n", prefix, target_name);
			if (!READER_CHECK(value.HasMember("width"), "%sTarget '%s': Must have numeric value 'width'\n", prefix, target_name)) return nullptr;
			if (!READER_CHECK(value["width"].IsNumber(), "%sTarget '%s': Value 'width' must be a number\n", prefix, target_name)) return nullptr;
			if (!READER_CHECK(value.HasMember("height"), "%sTarget '%s': Must have numeric value 'height'\n", prefix, target_name)) return nullptr;
			if (!READER_CHECK(value["height"].IsNumber(), "%sTarget '%s': Value 'height' must be a number\n", prefix, target_name)) return nullptr;
			width = uint16_t(value["width"].GetDouble());
			height = uint16_t(value["height"].GetDouble());
			break;
	}

	// "scale_reference": makes "scale" a fraction of a picture this many pixels wide instead of a
	// fraction of the window. A blur pyramid wants this - with plain window fractions its level
	// count is fixed, so the deepest level's texel spans a constant share of the WINDOW and its
	// reach across the picture therefore grows as the window shrinks, which is exactly what makes
	// a glow tuned in a window look wrong in fullscreen.
	uint16_t reference_width = 0;
	if (value.HasMember("scale_reference"))
	{
		if (!READER_CHECK(value["scale_reference"].IsNumber(), "%sValue 'scale_reference' must be a number\n", prefix)) return nullptr;
		const double requested = value["scale_reference"].GetDouble();
		if (!READER_CHECK(requested >= 1.0 && requested <= 16384.0, "%sValue 'scale_reference' must be 1..16384\n", prefix)) return nullptr;
		if (!READER_CHECK(mode == TARGET_STYLE_NATIVE, "%sTarget '%s': 'scale_reference' only applies to native-type targets\n", prefix, target_name)) return nullptr;
		reference_width = uint16_t(requested);
	}

	// "attachments": how many colour attachments this target carries (default 1, all one format).
	uint32_t attachments = 1;
	if (value.HasMember("attachments"))
	{
		if (!READER_CHECK(value["attachments"].IsNumber(), "%sValue 'attachments' must be a number\n", prefix)) return nullptr;
		const int requested = int(value["attachments"].GetDouble());
		if (!READER_CHECK(requested >= 1 && requested <= 4, "%sValue 'attachments' must be 1..4\n", prefix)) return nullptr;
		attachments = uint32_t(requested);
	}
	return chains.targets().create_target(std::move(target_name), format, width, height, xprescale, yprescale, mode, double_buffer, bilinear, scale, screen_index, attachments, reference_width);
}

bool target_reader::validate_parameters(const Value& value, const std::string &prefix)
{
	if (!READER_CHECK(value.HasMember("name"), "%sMust have string value 'name'\n", prefix)) return false;
	if (!READER_CHECK(value["name"].IsString(), "%sValue 'name' must be a string\n", prefix)) return false;
	if (!READER_CHECK(value.HasMember("mode"), "%sMust have string enum 'mode'\n", prefix)) return false;
	if (!READER_CHECK(value["mode"].IsString(), "%sValue 'mode' must be a string (what screens does this apply to?)\n", prefix)) return false;
	if (!READER_CHECK(!value.HasMember("bilinear") || value["bilinear"].IsBool(), "%sValue 'bilinear' must be a boolean\n", prefix)) return false;
	if (!READER_CHECK(!value.HasMember("doublebuffer") || value["doublebuffer"].IsBool(), "%sValue 'doublebuffer' must be a boolean\n", prefix)) return false;
	if (!READER_CHECK(!value.HasMember("user_prescale") || value["user_prescale"].IsBool(), "%sValue 'user_prescale' must be a boolean\n", prefix)) return false;
	if (!READER_CHECK(!value.HasMember("scale") || value["scale"].IsNumber(), "%sValue 'scale' must be a numeric value\n", prefix)) return false;
	return true;
}
