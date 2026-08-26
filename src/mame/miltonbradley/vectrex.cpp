// license:BSD-3-Clause
// copyright-holders:Mathis Rosenhauer
/*****************************************************************

Vectrex, conceived by Smith Engineering, manufactured by GCE.
GCE was acquired by Milton Bradley half a year later.

Mathis Rosenhauer
Christopher Salomon (technical advice)
Bruce Tomlin (hardware info)

*****************************************************************/

#include "emu.h"
#include "vectrex.h"

#include "cpu/m6809/m6809.h"
#include "machine/6522via.h"
#include "machine/nvram.h"
#include "sound/ay8910.h"
#include "video/vector.h"

#include "softlist_dev.h"
#include "speaker.h"


// A Vectrex address that selects no device leaves the data bus floating, so the CPU reads
// back whatever the previous bus cycle left there - not a fixed 0 or $ff. The 6809 core
// already runs its dead cycles as real reads at the addresses the hardware drives (a
// no-offset indexed read puts the post-operand PC on the bus, ,R+ and extended end on a
// $ffff VMA-off cycle), so simply handing back the last byte driven reproduces the
// measured behaviour, addressing mode and all.
//
// This is what keeps the Mine Storm split-child bug invisible on real hardware: the BIOS
// vector loop's control-byte read at $f42c is LDA ,X, whose dead cycle exposes $f42e - the
// $2f opcode of the BLE that follows it. $2f is positive, so the runaway scan started by
// the bad pointer $3408 exits after one iteration instead of grinding through RAM.
// See mame_doc/vectrex-openbus-testcart-dev.md and minestorm-split-child-draw-bug.md for
// the real-hardware measurements this reproduces.
uint8_t vectrex_state::open_bus_r()
{
	// The debugger has no bus cycle of its own, so showing it the live residual would
	// make memory views change under it for no reason.
	if (machine().side_effects_disabled())
		return 0x00;

	return m_maincpu->bus_data();
}

void vectrex_state::vectrex_map(address_map &map)
{
	// Catch-all first: everything the later entries do not claim is unconnected, and an
	// unconnected address reads back the data bus. Leaving a gap unmapped instead would
	// silently return the fixed unmap value, so the floor has to be explicit.
	map(0x0000, 0xffff).r(FUNC(vectrex_state::open_bus_r));
	map(0xc800, 0xcbff).ram().mirror(0x0400).share("gce_vectorram");
	map(0xd000, 0xd7ff).rw(FUNC(vectrex_state::via_r), FUNC(vectrex_state::via_w));
	map(0xe000, 0xffff).rom().region("maincpu", 0); // cart area is installed at machine_start
}

static INPUT_PORTS_START(vectrex)
	PORT_START("CONTR1X")
	PORT_BIT(0xff, 0x80, IPT_AD_STICK_X) PORT_MINMAX(0,0xff) PORT_SENSITIVITY(50) PORT_KEYDELTA(30)

	PORT_START("CONTR1Y")
	PORT_BIT(0xff, 0x80, IPT_AD_STICK_Y) PORT_MINMAX(0,0xff) PORT_SENSITIVITY(50) PORT_KEYDELTA(30) PORT_REVERSE

	PORT_START("CONTR2X")
	PORT_BIT(0xff, 0x80, IPT_AD_STICK_X) PORT_MINMAX(0,0xff) PORT_SENSITIVITY(50) PORT_KEYDELTA(30) PORT_PLAYER(2)

	PORT_START("CONTR2Y")
	PORT_BIT(0xff, 0x80, IPT_AD_STICK_Y) PORT_MINMAX(0,0xff) PORT_SENSITIVITY(50) PORT_KEYDELTA(30) PORT_REVERSE PORT_PLAYER(2)

	PORT_START("BUTTONS")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_BUTTON1) PORT_PLAYER(1)
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_BUTTON2) PORT_PLAYER(1)
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_BUTTON3) PORT_PLAYER(1)
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_BUTTON4) PORT_PLAYER(1)
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_BUTTON1) PORT_PLAYER(2)
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_BUTTON2) PORT_PLAYER(2)
	PORT_BIT(0x40, IP_ACTIVE_LOW, IPT_BUTTON3) PORT_PLAYER(2)
	PORT_BIT(0x80, IP_ACTIVE_LOW, IPT_BUTTON4) PORT_PLAYER(2)

	PORT_START("3DCONF")
	PORT_CONFNAME(0x01, 0x00, "3D Imager")
	PORT_CONFSETTING(0x00, DEF_STR(Off))
	PORT_CONFSETTING(0x01, DEF_STR(On))
	PORT_CONFNAME(0x02, 0x00, "Separate images")
	PORT_CONFSETTING(0x00, DEF_STR(No))
	PORT_CONFSETTING(0x02, DEF_STR(Yes))
	PORT_CONFNAME(0x1c, 0x10, "Left eye")
	PORT_CONFSETTING(0x00, "Black")
	PORT_CONFSETTING(0x04, "Red")
	PORT_CONFSETTING(0x08, "Green")
	PORT_CONFSETTING(0x0c, "Blue")
	PORT_CONFSETTING(0x10, "Color")
	PORT_CONFSETTING(0x14, "White")
	PORT_CONFNAME(0xe0, 0x80, "Right eye")
	PORT_CONFSETTING(0x00, "Black")
	PORT_CONFSETTING(0x20, "Red")
	PORT_CONFSETTING(0x40, "Green")
	PORT_CONFSETTING(0x60, "Blue")
	PORT_CONFSETTING(0x80, "Color")
	PORT_CONFSETTING(0xa0, "White")
	PORT_CONFNAME(0x300, 0x000, "3D color disc")
	PORT_CONFSETTING(0x000, "Auto (cart)")
	PORT_CONFSETTING(0x100, "Mine Storm (G-R-B)")
	PORT_CONFSETTING(0x200, "Narrow / Coaster (R-G-B)")
	PORT_CONFNAME(0xc00, 0x000, "3D stereo out")
	PORT_CONFSETTING(0x000, DEF_STR(Off))
	PORT_CONFSETTING(0x400, "Side-by-side")
	PORT_CONFSETTING(0x800, "Anaglyph (color)")
	PORT_CONFNAME(0x1000, 0x000, "3D anaglyph swap L/R")
	PORT_CONFSETTING(0x0000, DEF_STR(Off))   // left = red, right = cyan
	PORT_CONFSETTING(0x1000, DEF_STR(On))    // left = cyan, right = red

	PORT_START("3DPHASE")
	PORT_ADJUSTER(42, "3D color phase") // colour segments vs index hole; 50 = stock, <50 earlier, >50 later (coarse, 0.005/step)

	PORT_START("3DPHASEF")
	PORT_ADJUSTER(50, "3D color phase (fine)") // sub-step trim, 50 = none; spans one coarse step (0.0001/step)

	PORT_START("XSKEW")
	PORT_ADJUSTER(52, "X skew delay") // Y-axis lead -> Vectrex glyph slant; ~80ns/step (0 = upright, ~51 = ~4us default, 100 = ~8us)

	PORT_START("BEAMINFL")
	PORT_ADJUSTER(60, "Beam draw-time influence") // 0 = flat intensity, 100 = fully draw-time shaped (short=dim, long=bright)
	PORT_START("BEAMCURVE")
	PORT_ADJUSTER(50, "Beam draw-time curve")     // draw-time (dt) saturation gentleness (g = adj/50; 50 = 1.0)
	PORT_START("BEAMMAX")
	PORT_ADJUSTER(50, "Beam max energy")          // per-unit-area phosphor saturation ceiling (max = adj/100 x 8; 50 = 4.0)
	PORT_START("DOTREF")
	PORT_ADJUSTER(60, "Dot dwell ref (x5 us)")    // parked-dot dwell normalizer T_ref = adj x 5us (60 = 300us); the I x dt dazzle contour stays linear below it
	PORT_START("DOTCURVE")
	PORT_ADJUSTER(80, "Dot dwell curve")          // parked-dot dwell saturation exponent (g = adj/50; 50 = 1.0 = linear I x dt region)
	PORT_START("DOTMAX")
	PORT_ADJUSTER(20, "Dot max energy")           // parked-dot energy ceiling (max = adj/100 x 16; 20 = 3.2), independent of the line ceiling
	PORT_START("BEAMDWELL")
	PORT_ADJUSTER(10, "Dwell accum limit")   // cap same-spot pile-up; 0 = only first dot, 100 = ~unlimited (cap = adj/100 x 16)
	PORT_START("BEAMSPEED")
	PORT_ADJUSTER(6, "Stroke speed norm")    // stroke-mode inverse-speed normalizer (x = (1/speed) * adj*90000)
	PORT_START("BRIGHT")
	PORT_ADJUSTER(50, "Brightness knob")     // master beam brightness, like the knob on the back of the real console: 50 = normal (x1), 100 = x2 intensity
	PORT_START("SPOTKILL")
	PORT_CONFNAME(0x01, 0x01, "Spot killer (deflection protect)")   // cut the beam when deflection stops, modelling the real Vectrex CRT burn-in protection
	PORT_CONFSETTING(0x00, DEF_STR(Off))
	PORT_CONFSETTING(0x01, DEF_STR(On))
	PORT_START("SPOTKMS")
	PORT_ADJUSTER(25, "Spot killer time")    // travel measurement window (adj x 10 ms; 25 = 250 ms)
	PORT_START("SPOTKRNG")
	PORT_ADJUSTER(15, "Spot killer travel")  // beam must travel at least this per window to stay alive (adj = full draw WIDTHS per window; normal drawing = hundreds, parked/jittering beams ~0)
	PORT_START("OBJKNEE")
	PORT_ADJUSTER(75, "Object lift knee")    // intensity (Z) where the per-object brightness/width lift starts (knee = adj/100)
	PORT_START("OBJSHARP")
	PORT_ADJUSTER(50, "Object lift sharp")   // lift curve sharpness past the knee (sharp = adj/25; 50 = 2.0)
	PORT_START("OBJMAX")
	PORT_ADJUSTER(75, "Object lift max")     // max multiplier at full intensity for bullets/explosions (max = adj/25; 25 = 1.0 off, 75 = 3.0)
	PORT_START("OBJSTAR")
	PORT_ADJUSTER(75, "Object star lift")    // extra multiplier for parked dots / stars (star = adj/50; 50 = 1.0 off, 75 = 1.5)

	PORT_START("LPENCONF")
	PORT_CONFNAME(0x03, 0x00, "Lightpen")
	PORT_CONFSETTING(0x00, DEF_STR(Off))
	PORT_CONFSETTING(0x01, "left port")
	PORT_CONFSETTING(0x02, "right port")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_BUTTON5) PORT_CODE(MOUSECODE_BUTTON1)

	// Conditioned on the lightpen being plugged in. These are the only fields on the machine that
	// carry a crosshair, and crosshair_manager keys on ioport_field::enabled(), so without the
	// condition every Vectrex game draws a lightpen crosshair over itself - the machine ships with
	// the pen unplugged, and the controller is an analog stick that has no crosshair of its own.
	// via_cb2_w only reads these when LPENCONF selects a port, so conditioning them out changes
	// nothing else.
	PORT_START("LPENY")
	PORT_BIT(0xff, 0x80, IPT_LIGHTGUN_X)  PORT_CROSSHAIR(Y, 1, 0, 0) PORT_MINMAX(0,0xff) PORT_SENSITIVITY(35) PORT_KEYDELTA(1) PORT_PLAYER(1) PORT_CONDITION("LPENCONF", 0x03, NOTEQUALS, 0x00)

	PORT_START("LPENX")
	PORT_BIT(0xff, 0x80, IPT_LIGHTGUN_Y)  PORT_CROSSHAIR(X, 1, 0, 0) PORT_MINMAX(0,0xff) PORT_SENSITIVITY(35) PORT_KEYDELTA(1) PORT_REVERSE PORT_PLAYER(1) PORT_CONDITION("LPENCONF", 0x03, NOTEQUALS, 0x00)

INPUT_PORTS_END

void vectrex_base_state::vectrex_cart(device_slot_interface &device)
{
	device.option_add_internal("vec_rom",    VECTREX_ROM_STD);
	device.option_add_internal("vec_rom64k", VECTREX_ROM_64K);
	device.option_add_internal("vec_sram",   VECTREX_ROM_SRAM);
}

void vectrex_base_state::vectrex_base(machine_config &config)
{
	MC6809(config, m_maincpu, 6_MHz_XTAL); // 68A09

	/* video hardware */
	VECTOR(config, m_vector);
	// The beam events carry real sweep time: add_point is fed t0 = the previous event's end and
	// t1 = now, straight from the RAMP/VIA timing (vectrex_v.cpp update_vector), so a segment's
	// t1 - t0 is how long the beam actually took to draw it - the same contract avgdvg satisfies.
	// One list is one complete pass now that the generation only advances when the display window
	// moves (see screen_update), which the beam time window relies on.
	m_vector->set_avg_timing(true);
	SCREEN(config, m_screen, SCREEN_TYPE_VECTOR);
	m_screen->set_refresh_hz(60);
	m_screen->set_size(400, 300);
	m_screen->set_visarea(0, 399, 0, 299);
	m_screen->set_screen_update(FUNC(vectrex_base_state::screen_update));

	/* sound hardware */
	SPEAKER(config, "speaker").front_center();
	MC1408(config, m_dac, 0).add_route(ALL_OUTPUTS, "speaker", 0.25); // mc1408.ic301 (also used for vector generation)

	AY8912(config, m_ay8912, 6_MHz_XTAL / 4);
	m_ay8912->port_a_read_callback().set_ioport("BUTTONS");
	m_ay8912->port_a_write_callback().set(FUNC(vectrex_base_state::psg_port_w));
	m_ay8912->add_route(ALL_OUTPUTS, "speaker", 0.2);

	/* via */
	MOS6522(config, m_via6522_0, 6_MHz_XTAL / 4);
	m_via6522_0->readpa_handler().set(FUNC(vectrex_base_state::via_pa_r));
	m_via6522_0->readpb_handler().set(FUNC(vectrex_base_state::via_pb_r));
	m_via6522_0->writepa_handler().set(FUNC(vectrex_base_state::via_pa_w));
	m_via6522_0->writepb_handler().set(FUNC(vectrex_base_state::via_pb_w));
	m_via6522_0->ca2_handler().set(FUNC(vectrex_base_state::via_ca2_w));
	m_via6522_0->cb2_handler().set(FUNC(vectrex_base_state::via_cb2_w));
	m_via6522_0->irq_handler().set(FUNC(vectrex_base_state::via_irq));
}

void vectrex_state::vectrex(machine_config &config)
{
	vectrex_base(config);

	m_maincpu->set_addrmap(AS_PROGRAM, &vectrex_state::vectrex_map);

	vectrex_cart_slot_device &slot(VECTREX_CART_SLOT(config, "cartslot"));
	vectrex_cart(slot);

	/* software lists */
	SOFTWARE_LIST(config, "cart_list").set_original("vectrex");
}

ROM_START(vectrex)
	ROM_REGION(0x2000,"maincpu", 0)
	ROM_SYSTEM_BIOS(0, "bios0", "exec rom")
	ROMX_LOAD("exec_rom.bin", 0x0000, 0x2000, CRC(ba13fb57) SHA1(65d07426b520ddd3115d40f255511e0fd2e20ae7), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS(1, "bios1", "exec rom intl 284001-1")
	ROMX_LOAD("exec_rom_intl_284001-1.bin", 0x0000, 0x2000, CRC(6d2bd167) SHA1(77a220d5d98846b606dff608f7b5d00183ec3bab), ROM_BIOS(1) )

//  The following fastboots are listed here for reference and documentation
//  ROM_SYSTEM_BIOS(2, "bios2", "us-fastboot hack")
//  ROMX_LOAD("us-fastboot.bin", 0x0000, 0x2000, CRC(a6e4dac4) SHA1(e0900be6d6858b985fd7f0999d864b2fceaf01a1), ROM_BIOS(2) )
//  ROM_SYSTEM_BIOS(3, "bios3", "intl-fastboot hack")
//  ROMX_LOAD("intl-fastboot.bin", 0x0000, 0x2000, CRC(71dcf0f4) SHA1(2a257c5111f5cee841bd14acaa9df6496aaf3d8b), ROM_BIOS(3) )

ROM_END


/*****************************************************************

  RA+A Spectrum I+

  The Spectrum I+ was a modified Vectrex. It had a 32K ROM cart
  and 2K additional battery backed RAM (0x8000 - 0x87ff). PB6
  was used to signal inserted coins to the VIA. The unit was
  controlled by 8 buttons (2x4 buttons of controller 1 and 2).
  Each button had a LED which were mapped to 0xa000.
  The srvice mode can be accessed by pressing button
  8 during startup. As soon as all LEDs light up,
  press 2 and 3 without releasing 8. Then release 8 and
  after that 2 and 3. You can leave the screen where you enter
  ads by pressing 8 several times.

  Character matrix is:

  btn| 1  2  3  4  5  6  7  8
  ---+------------------------
  1  | 0  1  2  3  4  5  6  7
  2  | 8  9  A  B  C  D  E  F
  3  | G  H  I  J  K  L  M  N
  4  | O  P  Q  R  S  T  U  V
  5  | W  X  Y  Z  sp !  "  #
  6  | $  %  &  '  (  )  *  +
  7  | ,  -  _  /  :  ;  ?  =
  8  |bs ret up dn l  r hom esc

  The first page of ads is shown with the "result" of the
  test. Remaining pages are shown in attract mode. If no extra
  ram is present, the word COLOR is scrolled in big vector!
  letters in attract mode.

*****************************************************************/

void raaspec_state::raaspec_map(address_map &map)
{
	map(0x0000, 0x7fff).rom();
	map(0x8000, 0x87ff).ram().share("nvram");
	map(0xa000, 0xa000).w(FUNC(raaspec_state::raaspec_led_w));
	map(0xc800, 0xcbff).ram().mirror(0x0400).share("gce_vectorram");
	map(0xd000, 0xd7ff).rw(FUNC(raaspec_state::via_r), FUNC(raaspec_state::via_w));
	map(0xe000, 0xffff).rom();
}

static INPUT_PORTS_START(raaspec)
	PORT_START("LPENCONF")
	PORT_START("LPENY")
	PORT_START("LPENX")
	PORT_START("3DCONF")
	PORT_START("BUTTONS")
	PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_BUTTON1)
	PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_BUTTON2)
	PORT_BIT(0x04, IP_ACTIVE_LOW, IPT_BUTTON3)
	PORT_BIT(0x08, IP_ACTIVE_LOW, IPT_BUTTON4)
	PORT_BIT(0x10, IP_ACTIVE_LOW, IPT_BUTTON5)
	PORT_BIT(0x20, IP_ACTIVE_LOW, IPT_BUTTON6)
	PORT_BIT(0x40, IP_ACTIVE_LOW, IPT_BUTTON7)
	PORT_BIT(0x80, IP_ACTIVE_LOW, IPT_BUTTON8)
	PORT_START("COIN")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_COIN1)
INPUT_PORTS_END


void raaspec_state::raaspec(machine_config &config)
{
	vectrex_base(config);

	m_maincpu->set_addrmap(AS_PROGRAM, &raaspec_state::raaspec_map);

	NVRAM(config, "nvram", nvram_device::DEFAULT_ALL_0);

	m_via6522_0->readpb_handler().set(FUNC(raaspec_state::s1_via_pb_r));
}

ROM_START(raaspec)
	ROM_REGION(0x10000,"maincpu", 0)
	ROM_LOAD("spectrum.bin", 0x0000, 0x8000, CRC(20af7f3f) SHA1(7ce85db8dd32687ad7629631ae113820371faf7c))
	ROM_LOAD("exec_rom.bin", 0xe000, 0x2000, CRC(ba13fb57) SHA1(65d07426b520ddd3115d40f255511e0fd2e20ae7))
ROM_END

/***************************************************************************

  Game driver(s)

***************************************************************************/

//   YEAR  NAME       PARENT    COMPAT   MACHINE   INPUT     STATE          INIT        MONITOR  COMPANY                         FULLNAME
CONS( 1982, vectrex,  0,        0,       vectrex,  vectrex,  vectrex_state, empty_init,          "General Consumer Electronics", "Vectrex" , ROT270)

GAME( 1984, raaspec,  0,                 raaspec,  raaspec,  raaspec_state, empty_init, ROT270,  "Roy Abel & Associates",        "Spectrum I+", MACHINE_NOT_WORKING ) //TODO: button labels & timings, a mandatory artwork too?
