# VecBeamMAME FAQ

日本語: [faq.ja.md](faq.ja.md)

Questions that come up while using VecBeamMAME, and their answers.
The slider list is in [Added parameters](added-parameters.md), what goes in the
ini in [Startup options](startup-options.md), and HDR in
[HDR settings](hdr-settings.md).

---

## Is this based on real hardware?

It is checked and tuned against the machines here.

| Target | Reference used | Main titles checked |
|---|---|---|
| Colour vector | Amplifone '19 monitor | ATARI STAR WARS |
| Colour vector | Wells-Gardner WG6100 monitor | MAJOR HAVOC |
| Monochrome vector | A real Vectrex | — |

**No instruments were used.**  This is comparison by eye.  No photometer and no
scope has been put on any of it, so nothing here is guaranteed to be correct as
a number.  It is chased by comparing against photographs, video and memory.

## How close does it get to a real vector monitor?

Colour feels like it has come a reasonable way.  Monochrome is nowhere.
Perhaps with a display that reaches 3,000 nits and refreshes at 360 Hz?

## It looks tame next to video of a vector monitor

Filming a CRT makes it look spectacular, more or less always.  Enormous bloom
appears and the bright parts blow out to white.  **Looking at the real thing
with your own eyes does not do that.**

Matching the real thing alone runs into a different problem, though: a display
cannot reproduce the CRT's local peak brightness.  So the picture is corrected
some of the way back toward filmed video and toward memory.

## I am not satisfied with the picture

Monitors vary, and so do operating systems, so this is unavoidable.  Some
adjustment by hand is expected.  Starting from the macro parameters is quickest
(see below).

## There are too many parameters

Most of the time the **macro parameters** are enough.  Their slider names begin
with `[M]`, and each moves a group of related sliders together - `[M] Beam
Brightness`, for example.  There are too many of them, yes; cutting them down
properly is its own difficult job.

→ The first section of [Added parameters](added-parameters.md) is the macros.

## Why does `beam_width_min` do nothing, in the ini or in the Tab menu?

**The width options among stock MAME's "CORE VECTOR OPTIONS" have no effect while
a bgfx vector chain is in use**, because the drawing has been replaced by the
chain's analytic line renderer.  Writing them in the ini and dragging them in
Tab -> Slider Controls both change nothing.

| ini option | Name in the Tab menu | Use instead |
|---|---|---|
| `beam_width_min` / `beam_width_max` | Beam Width Minimum / Beam Width Maximum | The **chain sliders** of the same names (shown with Advanced on), or `[M] Beam Width` |
| `beam_dot_size` | Beam Dot Size | `Point Width Scale`, or `[M] Point Size` |
| `beam_intensity_weight` | Beam Intensity Weight | `Brightness Threshold (T)` and `Brightness Sigmoid` |
| `flicker` | Vector Flicker | `[M] Beam/Supply Sim` and the chain's `Cyclic Flicker *` (see below) |

**The confusing part is that the names are identical.**  Open Tab -> Slider
Controls and `Beam Width Minimum` / `Beam Width Maximum` appear in two places.
They are different things.

- The **upper** ones - `Vector Flicker` through `Beam Intensity Weight`, right
  after the screen position and size adjusters - are stock MAME's.  **Moving
  them does nothing.**
- The **lower** ones, among the bgfx chain sliders, are the ones that work.
  **Use those.**

Stock MAME's values only set the line width on the render primitive, which the
analytic renderer never reads; beam width comes from the chain slider's value.

The shipped `ini/presets/vector.ini` and `vector-mono.ini` still carry these
lines.  That is deliberate: without a bgfx vector chain - running `-video`
on something other than bgfx, for instance - they work as they always did.

`flicker` works in any configuration (the vector device drops each vector's
intensity at random).  But that is a different thing from the chain's cyclic
flicker, which reproduces a real machine carrying vectors it could not finish
into the next frame, and with both active they apply twice.  **The shipped ini
presets therefore set `flicker 0.00`.**  Leave it there.

## Does beam timing exist for every game?

**No.**  What VecBeamMAME is built on is timing information - when the beam drew
a line, and how long it took.

**This is a statement about MAME's emulation, not about the hardware.**  A real
vector monitor draws with a beam that takes time, whatever the board, so the
timing physically exists everywhere.  What differs is **whether that board's
vector generator timing has been modelled in MAME**.  "None" in the table below
does not mean the board had no timing; it means **MAME does not currently
produce it**.

| Vector generator | Titles | Timing MAME supplies |
|---|---|---|
| **Atari AVG / DVG** | STAR WARS, EMPIRE STRIKES BACK, TEMPEST, MAJOR HAVOC, BATTLEZONE, RED BARON, ASTEROIDS, ASTEROIDS DELUXE, LUNAR LANDER, GRAVITAR, SPACE DUEL, BLACK WIDOW, QUANTUM, TOMCAT, **OMEGA RACE** (a Bally game, using Atari's DVG) | **Yes** — a real sweep duration per segment |
| **Vectrex** | All Vectrex software | **Yes** — its own model |
| Cinematronics CCPU | RIP OFF, SOLAR QUEST, STAR CASTLE, WARRIOR, ARMOR ATTACK, BARRIER, SUNDANCE, SPACE WARS, SPEED FREAK, STAR HAWK, TAIL GUNNER, WAR OF THE WORLDS, BOXING BUGS, DEMON | **Effectively none** — a timestamp arrives, but `t0 == t1`: no sweep duration is modelled |
| Sega G80 vector | TAC/SCAN, STAR TREK, ZEKTOR, SPACE FURY, ELIMINATOR | **None** (not modelled) |
| Others | AZTARAC, COSMIC CHASM, VERTIGO | **None** (not modelled) |

The Atari AVG / DVG timing comes from running the vector generator's state
machine cycle by cycle to produce a sweep duration per segment
(`src/devices/video/avgdvg.cpp`).  So it **can** be done; the other boards lack
it only because nobody has written that timing model yet.

### What timing buys you

| Feature | With timing | Without |
|---|---|---|
| **Energy model** (speed and dwell to brightness) | A line the beam crossed slowly, or a dot it rested on, is brighter | **Inactive.** Brightness is whatever intensity the game asked for |
| **Z rise response** (`z_rise_tau`) | A briefly parked dot ends before Z has risen, so it is dimmer and thinner | Inactive - every dot is equally bright |
| **Beam time window** (`beam_window`) | One present deposits part of the sweep and the rest is left to the phosphor: the flicker a real machine has | **Does not engage.** A whole frame arrives at once |
| **Strike flash** (`beam_flash_ms`) | Only the slice just deposited is lifted | Inactive - it is gated on the window |
| **Cyclic flicker** (`flicker_*`) | Vectors are bucketed by sweep time and a busy frame drops some | Inactive |
| **Clip-window sample-and-hold** (`vector_window_sim`) | Reproduces the MAJOR HAVOC / BATTLEZONE window circuit's imperfections | The hardware that has the circuit is Atari AVG only, so this always applies |
| **MVEC recording** | Recorded with sweep timing, and replayed with the same time structure | Recorded as an event list with no timing |

So on a machine where the timing does not arrive, **the display side -
persistence, glow, halation, colour, geometry - still works exactly the same,
but the layer that derives brightness from beam movement and the layer that
slices one sweep by time both drop out entirely.**  It still looks better than
stock MAME, but it is not what STAR WARS or a Vectrex looks like here.

**No setting can add it.**  The renderer does what it can with what arrives, so
gaining it means writing the vector generator's emulation.  Which also means
that if more boards get one, the same reproduction starts working there too.

## Does this fix any bugs in MAME itself?

**Yes.**  Working on the display turned up faults that apply to stock MAME 0.289
as well, and those are fixed here.  All of them are small enough to report
upstream individually.

### Bugs in 0.289

**The size of the AVI readback buffer** - while recording, the readback buffer
took its width from the new dimensions but **its height from the old ones**.
Both the texture being read back and the destination bitmap use the new
dimensions, so a window that had grown overflowed the heap by
`(new_h - old_h) x new_w x 4` bytes.  It is currently unreachable - the two are
always equal by the time it is called - but what guarantees that is a non-local,
undocumented condition, so it was fixed.

**`bgfx_screen_chains` overwriting cfg** - the loading side lets a chain given
on the command line take precedence over cfg, but the saving side had no
matching check, so **a temporary override was burned into cfg.**  Run
`-bgfx_screen_chains vector-monochrome` once for a test and that machine starts
in monochrome from then on.  The check now lives in one place and both sides
share it.

### Vectrex hardware behaviour

**Open bus** - reads from unconnected addresses return what is left on the data
bus, not a fixed value.  On real hardware Mine Storm's out-of-range vector
drawing bug clears in one iteration and is invisible; returning `0` or `$FF`
amplified it into a freeze lasting seconds.  Unconnected is now the default,
and writes latch as the residual value too, since the CPU drives the bus when
it writes.

**The light pen crosshair** - a crosshair was drawn with no pen plugged in.

**3-D Imager eye and colour wheel sync** - `psg_port_w` re-armed the eye timer
on every PWM edge with the right eye hardcoded (param 2), clobbering the
one-shot that switches to the left eye half a rotation later.  The index timer
the game synchronises to was also left at its 1 Hz startup value and never
tracked the motor.  The result is swapped colours, or a picture stuck on one
eye.  The index pulse is now re-armed to the real rotation frequency, phase
preserved, and the eye switch and colour segments are derived from it.  **The
same code is still in 0.289's source.**

## Which chain is used?

Four chains ship, and **colour, monochrome and Vectrex machines each select the
matching one automatically.**

| Chain | For |
|---|---|
| `vector-color` | Colour vector machines (STAR WARS, MAJOR HAVOC and so on) |
| `vector-monochrome` | Monochrome vector machines |
| `vector-vectrex` | Vectrex |
| `default-vector` | Minimal fallback - when the above fails to load, and for unknown vector hardware |

To choose one yourself: `-video bgfx -bgfx_screen_chains <chain-name>`.
The former `*-balanced` names are migrated to the standard ones on load, so
**existing configuration files keep working.**

## What monitor is used as the reference?

The Liquid Retina XDR display built into a MacBook Pro (M5), with a TCL 32R84
(DisplayHDR 1400, mini-LED HVA panel) as an external monitor alongside it.

## What monitor do you recommend?

**DisplayHDR 1000 or better.**  OLED panels are untested, so no opinion there.

## The screen is dark

Check these in order.

1. **Turn the monitor's brightness to maximum.**  Switch off automatic
   brightness if it is on.
2. **Check that HDR is actually enabled.**  If it cannot be, the renderer falls
   back to SDR and that is darker.
3. Set `bgfx_hdr_display_peak` and `bgfx_hdr_paper_white` to match the monitor's
   peak brightness.
4. Games differ by a factor of two in the beam drive they ask for.
   `Brightness Threshold (T)` decides which drive level reaches full brightness,
   so lower it for a game that renders dark.

The monitor's limits cannot be exceeded.

→ [HDR settings](hdr-settings.md); the options themselves are in
[Startup options](startup-options.md).

## Something looks wrong

**A graphics driver setting may be adjusting brightness on its own.**  If any
such feature is enabled - contrast enhancement, dynamic brightness or contrast -
turn it off.

Vector scan is an extreme image, lines or dots on black, and automatic
brightness correction handles it badly.

## Why does distorting the CRT not distort the vector image with it?

**Because that is correct.**  Vector or raster, **an image on a CRT does not distort while you
are looking at it straight on.**  However spherical the face of the tube is, the picture drawn on
it does not bend with it.  Bending is what you see from an angle.

`Tube Quadric Distortion` describes **the shape of the glass**, not the deflection.  It drives
reflections, highlights, and the rounding of the tube's edge.

**To distort the beam instead**, use `Vector Pincushion X (Quad)` and `Vector Pincushion Y
(Quad)`, which model non-linearity in the deflection - the equivalent of a real deflection
circuit being out of adjustment.  Those do bend the picture itself.

### When the artwork does not line up

With `[M] Monitor/Glass Sim` on, the picture is scaled to sit inside the tube face:
`tube_face_scale` to 0.98 and `vector_image_scale` to 0.94.  **If that puts it out of register
with an artwork bezel, turn `[M] Monitor/Glass Sim` off.**  (The shadow mask, edge defocus, tube
vignetting and bezel reflection go with it.)

## It runs slowly

There are options for running on a slow PC.  One word gets you started:

```
-vector_quality low
```

`low` / `medium` / `high` set internal resolution, output resolution, the beam
time window and the present rate together.

| | Internal | Output | Beam time window | Present rate |
|---|---|---|---|---|
| `high` | 1.0 | 1.0 | on | the monitor's refresh |
| `medium` | 0.75 | 0.75 | on | **pinned at 60 Hz** |
| `low` | 0.5 | 0.5 | **off** | none (no present loop at all) |

On the slowest Windows PC here, a **Surface Pro 4** (Intel HD 520), settings
equivalent to `-vector_quality low` hold 41-42 presents per second and 100%
speed through the Death Star explosion in STAR WARS.  Anything slower than that
will struggle.

**Before touching any setting, check the PC's power mode.**  Power saving,
running on battery, or thermal throttling will cap the CPU and GPU clocks, and
that alone costs several times the speed.  Plug the adapter in and put the power
plan on high performance before measuring.  That has genuinely been the cause of
"it is slow" here.

**If `low` is still not enough, lower `bgfx_render_scale` further.**  The passes
that scale with internal resolution are about two thirds of a frame, so this is
where the time is.  0.5 to 0.4 works out at roughly 30% less.

Games whose driver refresh rate is around 60 Hz (GRAVITAR, TEMPEST, BATTLEZONE
and so on) ask for 1.5 times the frames STAR WARS does at 41 Hz, so the same
settings may not fit.

**Watch out for monitors above 60 Hz.**  With the beam time window on, the
present rate follows the monitor's refresh, so a 144 Hz panel runs the phosphor,
composite and monitor chain 144 times a second - 2.4x what 60 Hz costs.  On a
fast machine that buys picture quality; on a slow one it is just load.  That is
why `-vector_quality medium` pins the present rate at 60, and if you want to
stay on `high` while capping it, pass `-vector_present_rate 60` directly.

Only the compositing side scales this way.  Building the vectors, and the
auxiliary passes behind glow and the rest, happen per source frame and do not
follow the refresh rate.

## Why is there no Mac binary?

There are various reasons.  The official MAME project does not distribute Mac
binaries either.  Compiling is not difficult
([How to compile? in README.md](../../README.md#how-to-compile)).

## Does it work on Linux?

**It should, but it is untested.**  These do not work at present:

- **HDR / EDR output** — implemented for Windows and macOS only (excluded at
  compile time).
- **Monitor refresh rate detection** — Windows and macOS only, so
  `-vector_present_rate auto` **stays at 60 Hz**.  On a high-refresh panel,
  specify it explicitly: `-vector_present_rate 144`.
