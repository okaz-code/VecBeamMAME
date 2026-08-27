// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    mame.cpp

    Specific (per target) constants

****************************************************************************/

#include "emu.h"
#include "main.h"

// VecBeamMAME is a fork and ships as its own program, so it carries its own names: the executable
// is vbmame (scripts/src/main.lua), the ini it reads and -createconfig writes is vbmame.ini, and on
// SDL/Mac the per-user config directory becomes ~/.vbmame. That is the point of renaming - a stock
// MAME can live in the same place without the two sharing an ini full of options the other does not
// know. Save states are unaffected: their magic is a separate constant (STATE_MAGIC_NUM).
#define APPNAME                 "VecBeamMAME"
#define APPNAME_LOWER           "vbmame"
#define CONFIGNAME              "vbmame"
#define COPYRIGHT               "Copyright MAMEdev and contributors\nhttps://mamedev.org\nVecBeamMAME fork: https://github.com/okaz-code/VecBeamMAME"
#define COPYRIGHT_INFO          "Copyright MAMEdev and contributors; VecBeamMAME fork by okaz-code"

const char * emulator_info::get_appname() { return APPNAME;}
const char * emulator_info::get_appname_lower() { return APPNAME_LOWER;}
const char * emulator_info::get_configname() { return CONFIGNAME;}
const char * emulator_info::get_copyright() { return COPYRIGHT;}
const char * emulator_info::get_copyright_info() { return COPYRIGHT_INFO;}
