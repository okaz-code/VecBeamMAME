# Startup Options Reference

For: VecBeamMAME (based on MAME 0.289)

**The full reference for the 27 startup options VecBeamMAME adds** that stock
MAME 0.289 does not have.  All of them can be given on the command line or in
an ini.

For the chain sliders - the ones you change while running, which are saved to
cfg - see the [added parameters list](added-parameters.md).  **This document
covers only what is decided at startup.**

---

## 1. Writing an ini file

### 1-0. Creating the ini

Start from a generated one.  **The VecBeamMAME executable is `vbmame`**, and
the ini it reads is **`vbmame.ini`** - a separate file from stock MAME's, so
both can live in the same folder.

```
vbmame -createconfig
```

`-cc` is the short form.  It writes `vbmame.ini` (carrying every option's
default) and `ui.ini` into the current directory.  Edit that file from then on.

**Coming from stock MAME, your existing `mame.ini` will not be read.**  Rename
it to `vbmame.ini`, or generate a fresh one with the command above and move the
lines you need across.  Per-game `cfg/` and `nvram/` can be used as they are.

### 1-0b. Stock MAME options that do nothing here

`beam_width_min` / `beam_width_max` / `beam_dot_size` / `beam_intensity_weight`
**have no effect while a bgfx vector chain is in use.**  Neither writing them in
the ini nor dragging the sliders of the same names in Tab -> Slider Controls
(stock MAME's, near the top of the list) changes anything.  Beam width comes
from the **chain sliders**, further down that list.  Use those.

`flicker` does work, but it doubles up with the chain's cyclic flicker, so the
shipped ini presets set `flicker 0.00`.  Leave it there.

See "Why does `beam_width_min` do nothing, in the ini or in the Tab menu?" in
the [FAQ](faq.md) for the details.

### 1-1. Format

One option per line, with no leading `-`.  Any amount of whitespace separates
the name from the value.  `#` starts a comment.

```ini
#
# CORE VECTOR OPTIONS
#
vector_beam_window        1
vector_present_rate       auto
bgfx_hdr                  1
bgfx_hdr_display_peak     auto
```

**Booleans** are written `1` / `0`.  The command line's `no` prefix, as in
`-novector_playback_overlay`, is not used in an ini; write
`vector_playback_overlay 0` instead.

**Path options** such as `vector_record` are unset by leaving the value empty.

### 1-2. Where ini files are looked for, and what wins

They are searched along `inipath` (`$HOME/.mame;.;ini;ini/presets` by default),
and **the ones lower in this table override the ones above**.

| ini | Applies to | Priority |
|---|---|---|
| `vbmame.ini` | Everything | 101 (weakest) |
| `debug.ini` | With the debugger | 102 |
| `horizont.ini` / `vertical.ini` | By screen orientation | 103 |
| `vector.ini` / `raster.ini` / `lcd.ini` etc. | **By screen type** | 104 |
| `source/<source file>.ini` | Per driver source file, e.g. `source/starwars.ini` | 105 |
| `<grandparent set>.ini` | | 106 |
| `<parent set>.ini` | | 107 |
| `<driver>.ini` | | 108 |
| `<set name>.ini` | e.g. `starwars.ini` | 109 |
| **Command line** | | **151 (strongest)** |

To affect vector machines only, write it in `vector.ini`; for Star Wars alone,
`starwars.ini`.

VecBeamMAME ships `ini/presets/vector.ini` and `ini/presets/vector-mono.ini`.
Since `ini/presets` is on `inipath`, creating `ini/vector.ini` overrides them.

### 1-3. Watch the priority 105 boundary

`bgfx_screen_chains` **counts as "explicitly specified" at priority 105
(`source/*.ini`) and above**: the chain selection saved in cfg is then neither
read nor written back.  Written in `vbmame.ini` (101) it is treated as an
ordinary default, and the cfg selection wins.

So:

- To set a chain **as a general preference** - write it in `vbmame.ini`
- To pin one **for a particular game** - `<set name>.ini` or the command line

---

## 2. Vector drawing core options

`-vector_*`.  These belong to the emulation side (`src/emu`,
`src/devices/video/vector.cpp`) and work with any renderer.

### `vector_quality` / alias `vecq`

| | |
|---|---|
| Type / default | string / **unset (changes nothing)** |
| Values | `high` / `medium` (alias `middle`) / `low` |

**A preset that sets the three options governing drawing cost together.**  It
lets you pick a load in one word, without having to remember
`bgfx_render_scale` / `bgfx_output_scale` / `vector_beam_window` individually.

| Value | `bgfx_render_scale` | `bgfx_output_scale` | `vector_beam_window` |
|---|---|---|---|
| `high` | `1.0` | `1.0` | on |
| `medium` | `0.75` | `0.75` | on |
| `low` | `0.5` | `0.5` | **off** |

What actually costs is the two resolution scales; the beam window rides on top
of them.  **The window is the first thing to drop on a machine without room to
spare**, so only `low` turns it off.

**A preset is a starting point, so anything explicit beats it.**  It fills in
**only those of the three that nobody has touched** (priority 0, still the
built-in default).  A value written anywhere - an ini (101 and up) or the
command line (151) - stays as written (see
[1-2](#1-2-where-ini-files-are-looked-for-and-what-wins)).  So
`-vector_quality low -vector_beam_window 1` means "half resolution, window on",
and with `bgfx_render_scale 1.0` in `vbmame.ini` even `low` will not lower the
resolution.

Anything other than `high` / `medium` / `low` warns and **the whole preset is
ignored** (all three stay as configured).

```
Unknown -vector_quality 'ultra'; expected high, medium or low. Ignoring.
```

`vector_quality` is itself an `src/emu` option, but two of the three things it
fills in are on the BGFX side (see
[4](#4-bgfx-renderer-options-added-by-vecbeam)), so **on renderers other than
BGFX only the window part has any effect.**

### `vector_overscan_x` / `vector_overscan_y`

| | |
|---|---|
| Type / default | float / `1.0` |

Zoom factor of the vector image about the centre of the screen.  **Below 1.0
the image shrinks, and beam excursions that would have gone off-screen come
into view.**

A real monitor can deflect somewhat beyond its visible face.  Where a game
deliberately swings the beam off-screen - explosions in Star Wars, outside the
window in Major Havoc - the edge of a real tube lights up.  Lower this when you
want to see that, or to check the beam that feeds the monitor glow.

Around `0.95` the edge behaviour becomes visible.  The image gets smaller, so
keep `1.0` for normal play.

### `vector_blank_leak`

| | |
|---|---|
| Type / default | float / `0.0` (off) |

Draws the path taken during blanking - the moves and retraces where
`intensity == 0` - at a very low brightness.

Real Z-axis blanking does not cut completely; a little leaks through.  A fast
jump dwells briefly and so comes out faint, a slow move darker (on devices
where `beam_energy` is `-1`, meaning not supplied, it is estimated from speed).

**At 0 the vertices are not emitted at all**, so it costs nothing.  The geometry
of blanked moves exists only on the device side, which is why this cannot be a
chain slider.  The colour used is AVG's STAT colour.

### `vector_beam_window` / alias `beamwin`

| | |
|---|---|
| Type / default | bool / **`1` (on)** |

**Each present deposits only the slice of the beam sweep that belongs to that
present.**  The rest is held by the phosphor's persistence.  This reproduces a
real CRT being drawn progressively across a frame, and in games with a long
sweep you can see the scan itself advancing.

There is no point in windowing if there is nothing to cut, so **while this
option is on, `vector_present_rate` is promoted to `auto` if it is still at its
default** (an explicit value is respected).

```
Vector presentation timer enabled at auto, initial 60 Hz (requested by vector_beam_window)
BGFX: beam time window active - sweep 29.90 ms over 12.50 ms windows (2.4 per sweep), scale 2.00
```

In games whose sweep fits inside a single window (asteroid, for one) there is
nothing to cut; it reports `inert` and falls back to per-frame drawing.

**Cost**: the present loop runs on every vector machine, so even titles where
the window does nothing spend 2-4x the vector CPU of the per-frame path.  Turn
it off with `-novector_beam_window`, or `-vector_present_rate 0`.

The chain also has `beam_window` / `beam_window_scale` sliders, and **both sides
must be on** for it to work: this option is the master switch at startup, the
sliders are the adjustment while running.

### `vector_window_sim`

| | |
|---|---|
| Type / default | bool / **`1` (on)** |

**Master switch for the whole sample-and-hold model of the clip window.**  It
exists to handle the four options below together: `-novector_window_sim`
**forces all four to 0**, whatever they were individually set to.

One switch, so that you can remove the model without remembering each one's
"off" value.  Use it to compare against an ideal hold - no droop, no memory, no
scatter, no offset.

### `vector_window_droop` / `vector_window_memory` / `vector_window_jitter` / `vector_window_bias`

| Option | Type | Default |
|---|---|---|
| `vector_window_droop` | float | `5.0` |
| `vector_window_memory` | float | `0.005` |
| `vector_window_jitter` | float | `0.0` (off) |
| `vector_window_bias` | float | `0.5` |

**These only affect machines that have this circuit.**  Major Havoc holds one
edge - the top of the scrolling region (the `ymin` clip) - and Battlezone holds
all four edges of the clip rectangle, in both cases with an analog switch and a
hold capacitor (the same LF13201, the same 1000pF).  These four options
reproduce that circuit's non-ideal behaviour.

**Only `window_jitter` defaults to 0 (off).**  It is scatter in the latched
value itself, and **the only one of the four that moves between frames**.  On
Major Havoc's single edge it matches the real machine, but on Battlezone all
four edges of the rectangle move and the frame appears to breathe, so it is
opt-in.

The other three - droop, dielectric absorption, offset - default to values
close to the real circuit.  They **do not move between frames**: they shift the
position by a fixed amount, or lean along the drawing order.  Remove them all
with `-novector_window_sim`, or individually by passing that option `0`.

| Option | What it models | Unit | Off value |
|---|---|---|---|
| `window_droop` | Droop of the held voltage | % of screen height per second | `0` (ideal hold) |
| `window_memory` | Dielectric absorption in the hold capacitor (pulled toward earlier samples) | 0..1 | `0` |
| `window_jitter` | Scatter per sample.  **Constant within a frame** | % of screen height | `0` |
| `window_bias` | Systematic offset (charge injection / comparator offset) | % of screen height, signed | `0` |

They do nothing in other games.

### `vector_present_rate` / alias `vecpresent`

| | |
|---|---|
| Type / default | string / `0` (off) |
| Values | `0` = off, `auto` = the monitor's refresh, `1`-`360` = a fixed Hz |

Re-presents the finished output at the given rate **without changing emulation
timing**.  The source frame period - the rate the game draws at - is unaffected.

`auto` reads the actual desktop refresh rate on Windows (to fractional
precision) and `NSScreen`'s maximum frame rate on macOS, then uses the nearest
integer Hz.  **There is no detection path on Linux**, so it lands on 60 Hz;
specify a value explicitly if you need something else.

Its purposes are to make phosphor decay look smooth on a high-refresh monitor,
and to act as the foundation for `vector_beam_window`.

### `vector_phosphor_rate` / alias `vecphosphor`

| | |
|---|---|
| Type / default | int (0-360) / `0` (unlimited) |

Caps how many times per second the heavy phosphor / monitor chain is updated
while re-presenting at a high rate.

**New source frames are never delayed.**  Only the intermediate presents reuse
the finished output.  Use it to free up GPU while keeping the look, when
something like `vector_present_rate 240` is too expensive.

### `vector_event_dump`

| | |
|---|---|
| Type / default | path / unset |

For debugging.  Writes timed beam events as CSV, one event per line.  Open
`tools/mvec-viewer/mvec-viewer.html` in a browser to visualise it.

---

## 3. MVEC beam event streams

Records and replays the **final vector list** produced by actually playing a
game.  Recording is one frame per emulated frame, and the stream is an
intermediate representation upstream of the renderer, so **running the same MVEC
through different renderer settings leaves the renderer as the only
difference.**

The format is specified in [[../mvec-format-v1]], the capture procedure in
[[../mvec-comparison-capture-procedure]].

### `vector_record`

| | |
|---|---|
| Type / default | path / unset |

Writes an MVEC stream.  It stores each frame's final beam events (x/y, colour,
intensity, beam_energy, t0/t1, cap_flags, and Vectrex-specific metadata), and
at the same time saves the final speaker mix as 16-bit PCM to
`<MVEC path>.wav`.

**Mutually exclusive** with `vector_playback`.  If a machine has several vector
devices, only the first is recorded.

Writing happens on its own thread, so the effect on emulation speed is small,
but **the files are large**: 66 bytes per point, about 500MB per minute on
starwars.

### `vector_playback`

| | |
|---|---|
| Type / default | path / unset |

Replays an MVEC stream.  The beam list, list generation and stale/timed flags
are all overridden with the recorded values, so **the recorded session is
reproduced deterministically, down to the CRT flicker and the timing model.**
If the `.wav` recorded alongside it is present, it replaces the game audio and
stays synchronised to the frame position.

It can be replayed through any chain, so one recording of real play gives you
**an accurate A/B calibration that does not have to be reproduced by hand.**
This is what the comparison videos are built on.

**Note**: playback advances one recorded frame per screen update.  If the
driver's rate at playback differs from the rate at recording the playback speed
will be wrong, and a warning is printed when they diverge.

### `vector_playback_start` / alias `vecstart`

| | |
|---|---|
| Type / default | int / `0` (from the beginning) |

**Where playback starts.**  The same seek that `Alt+G` performs is done before
the first frame is shown.  Investigating something that only happens thousands
of frames in no longer means typing a frame number by hand every time, which
makes **the measurement reproducible.**

```
MVEC: starting playback at frame 4200
```

**Frame numbers are 1-based**, counted the same way as the overlay display and
`Alt+G` input, so a number shown in the overlay can be passed straight through.
`-vector_playback_start 100` stops on the frame the overlay labels `100`, the
same place `Alt+G` with `100` lands.

Values of `0` or less mean unspecified and play from the start.  A value past
the end of the stream is **clamped to the last frame** rather than being an
error.

### `vector_playback_end` (formerly `vector_exit_after_playback`)

| | |
|---|---|
| Type / default | int (0-2) / `0` |

What to do on reaching the end of the stream.

| Value | Behaviour |
|---|---|
| `0` | Hold the last frame (does not go black) |
| `1` | Exit the program |
| `2` | Loop back to the beginning |

`1` is for automated video capture: both builds then end on the same frame
count.

**The old name `vector_exit_after_playback` remains as an alias, but it now
requires a value.**  Having gone from BOOLEAN to INTEGER, the valueless
command-line flag form `-vector_exit_after_playback` no longer works - and
written as `-vector_exit_after_playback -window` it **eats the following option
as its value** (with a warning).  Use `-vector_playback_end 1`.  In an ini,
`vector_exit_after_playback 1` still works as before.

### `vector_playback_overlay`

| | |
|---|---|
| Type / default | bool / **`1` (shown)** |

**The initial visibility** of the playback position overlay.  It is shown by
default, as before.

`-novector_playback_overlay` starts with it hidden, which is what you want when
capturing video.  The `Alt+O` toggle (`Option+O` on macOS) still works in both
directions afterwards, and the overlay is forced visible while a frame number is
being typed for `Alt+G` (a hidden modal input cannot be told apart from the
input being locked up).

Note that `-aviwrite` does not include the UI layer, so **the overlay was never
in the AVI anyway**.  This option is about what is on screen.

### Keys during playback

All of them are Alt (Option on macOS) plus a key.

| Key | Action |
|---|---|
| `Alt+P` | Pause / resume |
| `Alt+Left` / `Right` | +-1 frame |
| `Alt+PgUp` / `PgDn` | +-60 frames |
| `Alt+Home` / `End` | First / last frame |
| `Alt+G` | Type a frame number and jump (digits then Enter; Esc cancels, Backspace deletes) |
| `Alt+O` | Toggle the overlay |

Frame numbers are **1-based**, matching the overlay.

---

## 4. BGFX renderer options (added by VecBeam)

`-bgfx_*`.  These are OSD-side options (`src/osd/modules`).

### `bgfx_vec_line_shader`

| | |
|---|---|
| Type / default | string / `analytic` |
| Values | `classic` / `analytic` |

How vector lines are drawn.

- `analytic` - a Gaussian line integral.  A segment is evaluated analytically,
  so it stays continuous at sub-pixel widths and its endpoints join cleanly.
  **The default**
- `classic` - quad plus endpoint fans.  The older method

It is used only by chains that declare `vector_engine=analytic`.  Three of the
four shipped chains do: `vector-color`, `vector-monochrome` and
`vector-vectrex`.  **`default-vector` does not**, so this option has no effect
there (it takes the stock drawing path).

### `bgfx_vec_supersample`

| | |
|---|---|
| Type / default | int (1-2) / `1` |

Supersampling factor for the vector FBO.  `1` = window resolution, `2` = twice
that.

`analytic` line drawing is already analytic and does not alias much, but this
helps where thin lines crowd together.  It costs 4x, by area.

### `bgfx_render_scale`

| | |
|---|---|
| Type / default | float (0.1-1.0) / `1.0` |

**The scale of the internal vector rendering.**  The final BGFX output, the UI
and the artwork all stay at window resolution.

This is the escape hatch for running a heavy chain at a high resolution.  At
`0.5` the internal size halves and vector drawing and glow cost about a
quarter.  Lines do not get thinner - width normalisation was built not to
depend on resolution.

### `bgfx_output_scale`

| | |
|---|---|
| Type / default | float (0.25-1.0) / `1.0` |

**The resolution of the HDR composite**, as a fraction of the window.  The final
image is upscaled to the window.  Where `bgfx_render_scale` shrinks only the
vector interior, this shrinks the whole composite.

### `bgfx_hdr`

| | |
|---|---|
| Type / default | int (0-1) / **`1`** |

Attempts HDR10 / EDR output.  **It falls back to SDR when unavailable**, so
having it on by default causes no trouble on an SDR monitor or an unsupported
backend.

`0` forces SDR.

```
BGFX: HDR present path = macOS EDR (Metal, extended-linear RGBA16F)
BGFX: HDR present path = HDR10 (PQ / Rec.2020, RGB10A2)
BGFX: HDR present path = SDR fallback (HDR requested but unavailable)
```

The decision is made from `BGFX_CAPS_HDR10`, plus Metal and available EDR
headroom on macOS.  **The OpenGL and Vulkan backends do not raise the HDR10
capability, so they get SDR.**

### `bgfx_hdr_paper_white`

| | |
|---|---|
| Type / default | int / `200` (nits) |

The white level of the UI and menus in HDR mode.  **This is the UI's
brightness**, not the game screen's.  Adjust it when the UI is too bright or too
dim on an HDR display.

Under macOS EDR this value is the reference white, the denominator in the
headroom calculation.

### `bgfx_hdr_display_peak`

| | |
|---|---|
| Type / default | string / `auto` |
| Values | `auto` = ask the OS, a number = nits, `0` = use the chain's defaults |

The display's peak luminance.  **The defaults for `beam_peak_nits` and
`hdr_rolloff_max` are derived from it.**  Used only while `bgfx_hdr` is on.

`auto` has a detection path on Windows (DXGI) and macOS (NSScreen EDR) only.
**There is none on Linux**, so a number is needed there (though HDR does not
arrive on Linux in the first place, which makes it moot).

Automatically derived values are **deliberately not saved to cfg**.  Were they
saved, the next launch would restore that number from cfg after auto-detection
had run, turning a monitor-dependent calibration into a fixed value.  Move
either slider by one step and it no longer matches the automatic value, so it is
saved as usual.

### `bgfx_macos_force_composited`

| | |
|---|---|
| Type / default | bool / `1` |

macOS only.  Requests Core Animation compositing with a non-opaque Metal layer,
which bears on whether EDR headroom can be obtained.  Disable with
`-nobgfx_macos_force_composited`.

### `bgfx_macos_edr_diagnostics`

| | |
|---|---|
| Type / default | bool / `0` |

macOS only.  Logs the `CAMetalLayer` state and the raw EDR headroom once a
second.  For working out why HDR is not coming out as expected.

---

## 5. Worked examples

### 5-1. Normal play (HDR display, macOS)

`ini/vector.ini`:

```ini
vector_beam_window        1
vector_present_rate       auto
bgfx_hdr                  1
bgfx_hdr_display_peak     auto
bgfx_hdr_paper_white      200
```

### 5-2. Reducing the load

Try one word first:

```
-vector_quality low
```

That sets `render_scale` and `output_scale` to `0.5` and turns the beam window
off.  If it does too little or too much, try `medium` (`0.75`, window on) and
`high` (`1.0`, window on).

Written out individually:

```ini
vector_beam_window        0
vector_present_rate       0
bgfx_render_scale         0.5
bgfx_output_scale         0.5
bgfx_vec_supersample      1
```

Turning `vector_beam_window` off stops the present loop, cutting vector CPU to
between a half and a quarter.

Measured numbers on low-end machines - what the bottleneck is differs by machine
- are in [[../low-end-performance-results]], and how to read the logs is in
[[../bgfx-perf-log-reading]].

### 5-3. Recording a comparison video

```
-vector_playback <name>.mvec -vector_playback_end 1 -novector_playback_overlay \
  -skip_gameinfo -resolution 1920x1080 -sound none -aviwrite out.avi
```

`-skip_gameinfo` is **required**.  The startup information screen waits for a
key, and without it playback never advances, so not even
`-vector_playback_end 1` will end the run - it looks like a hang.

`-vector_playback_end 1` is **also required if you are using `-aviwrite`.**  Left
at the default `0` (hold the last frame), the held frame keeps being written to
the AVI after the end of the stream and **the AVI grows without stopping.**  One
of these reached 139 GB and filled the disk.  `-seconds_to_run` will not stop it
(playback advances one recorded frame per screen update).

### 5-4. Seeing the beam outside the screen

```ini
vector_overscan_x         0.95
vector_overscan_y         0.95
vector_blank_leak         0.05
```

### 5-5. The clip window circuit (Major Havoc / Battlezone)

`jitter` is off by default; add it only when you want the between-frame
movement.  All four edges of Battlezone's rectangle move, so the same value
shows up more strongly there than on Major Havoc.

```ini
# put this in mhavoc.ini / bzone.ini, not starwars.ini
vector_window_droop       2.0
vector_window_memory      0.15
vector_window_jitter      0.3
vector_window_bias        -0.2
```

---

## 6. Stock MAME options that mean something different here

### `bgfx_screen_chains`

Not an option VecBeam adds, but its behaviour has changed.

**Specified at priority 105 (`source/*.ini`) or above, the chain selection in
cfg is neither read nor written back.**  Stock MAME 0.289 wrote it without
reading it, so specifying a chain once for a test rewrote that machine's saved
settings (fixed here).

Chain names are **the filename part only**: for
`bgfx/chains/vector/vector-color.json` that is `vector-color`, not
`vector/vector-color`.

### `aviwrite`

Does not include the UI layer - overlays and menus do not appear in the AVI.
The AVI is also recorded in SDR, even with an HDR chain.
