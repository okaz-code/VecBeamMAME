# VecBeamMAME vector chains

日本語: [README.ja.md](README.ja.md)

The calibrated chains are the standard VecBeamMAME vector display configurations:

- `vector-color`
- `vector-monochrome`
- `vector-vectrex`
- `default-vector` (minimal safety fallback)

Select the appropriate chain from MAME's video options or with
`-video bgfx -bgfx_screen_chains <chain-name>`.  BGFX slider values are saved
per system in the MAME configuration files.

Colour, monochrome and Vectrex systems select their corresponding standard chain
automatically. If that chain is unavailable or fails to load, VecBeamMAME falls
back to `default-vector`. Unknown vector hardware also uses `default-vector`.

The former `*-balanced` names are accepted when loading existing configuration
files and are migrated to the standard names.

## Standard-chain calibration

The standard defaults include the current per-system calibration work rather
than requiring a saved configuration file:

- `vector-color` includes the *Star Wars* overload-driven HV droop
  calibration.
- `vector-vectrex` includes the Vectrex persistence, beam/point,
  focus, halation, glow-buffer, starburst, ambient, glow-tail, and two-sided
  printed-overlay calibration. Its overload geometric-width controls are
  independent from overload bloom by default. It also provides an isolated
  dwell-dot minimum size, phosphor-coloured post-glow, and a common room-light
  control for the CRT face, overlay and ordinary bezel artwork (excluding UI).
- `vector-monochrome` includes the *Asteroids* point, focus,
  halation-fill, starburst-length, and ambient calibration.

A system configuration file can still override these defaults.  Delete the
saved BGFX slider entries or reset the sliders in the UI to evaluate the chain
defaults directly.

The Vectrex defaults mirror the current reference `vectrex.cfg` calibration,
including phosphor RGB `[0.50, 0.70, 1.00]`, a `3.0 px` isolated dwell-dot
minimum, `0.50` room ambient, and the calibrated two-sided overlay response.

## Overload-driven HV droop

HV droop models EHT supply sag as global dimming and spot defocus.  It is not
driven by the total number or energy of ordinary vectors.  The renderer sums
only the energy above `Overload Threshold` for non-point line primitives:

```text
overload load =
    sum(max(line energy - overload threshold, 0)
        * line length / reference screen width)

droop load =
    clamp((peak-tracked overload load - HV Droop Overload Onset)
          / HV Droop Load Ref, 0, 1)
```

`HV Droop (dim+defocus)`
: Sets the maximum strength.  At a value of 1.0 and full load, the line deposit
  is dimmed by up to 40% and the spot sigma gains about 2.5 pixels at the
  1920-pixel reference width.

`HV Droop Overload Onset`
: Rejects isolated or low-level overload.  Loads at or below this value have
  exactly no global dimming or defocus.

`HV Droop Load Ref`
: Sets how much additional overload above the onset is needed to reach full
  droop.  Lower values make a mass-overload event reach the maximum sooner.

Only a chain with `Overdrive (hot core)` enabled can generate this load.  Dwell
points are excluded, so one hot bullet or stationary dot cannot dim the entire
screen.  The peak tracker decays by 0.82 per source frame, giving a short supply
recovery after a large event.

The `vector-color` starting values are:

```text
HV Droop (dim+defocus)   0.50
HV Droop Overload Onset  0.50
HV Droop Load Ref        5.00
```

Raise the onset if ordinary gameplay still contains enough isolated overloaded
lines to cause visible sag.  Lower the load reference if a large event such as
the Death Star explosion does not reach the desired strength.

## Glow, halation, and convergence scatter

`Glow Narrow` and `Glow Wide` provide the ordinary near and broad line glow.
The extended wide-glow reach now covers the broad local halo formerly produced
by the separate Convergence Bloom feature, so the local Convergence Bloom and
its six tuning controls have been removed.

`Convergence Global Bloom` remains available in the standard chains.  It
detects a large connected overload region and adds a broad full-face scatter;
compact local objects do not receive a separate convergence halo.  Its
`Convergence Global Coverage` control sets the scatter footprint.

Point-optics controls are grouped in physical order in the slider menu:

1. Halation gain and ring controls
2. Starburst gain, count, length, randomness, width, and angle
3. Ordinary analytic glow controls

Deflection Dynamics and its settle/damping controls have been removed from the
chains.  Vector geometry is therefore rendered from the source trajectory
without the optional renderer-side second-order deflection simulation.

## High-refresh presentation

VecBeamMAME can present vector output at the monitor refresh rate without
increasing the emulation or source vector frame rate:

```text
vector_present_rate auto
vector_phosphor_rate 0
```

`vector_present_rate` accepts the following values:

- `0` disables presentation-only updates.
- `auto` obtains the active monitor refresh rate after the BGFX window is
  initialized.  On Windows, the exact fractional desktop rate is queried and
  rounded to the nearest whole presentation rate (for example, 143.988 Hz is
  presented at 144 Hz).  On macOS, the window's `NSScreen` maximum frame rate
  is used (normally 60 Hz, or up to 120 Hz on a ProMotion display).
- A number from 1 to 360 selects a fixed presentation rate.

Additional presents reuse the most recently completed vector output.  They do
not run another emulated machine frame, alter game speed, or make
`-nothrottle` depend on the presentation rate.

`vector_phosphor_rate` optionally limits the expensive temporal
phosphor/monitor chain while presentation continues at the requested rate:

- `0` is the default and updates the chain on every present.
- A number from 1 to 360 caps chain updates to that rate.  New source vector
  frames are never delayed; intervening presents reuse the completed output.

Short-dwell points and line caps use a non-persistent buffer.  Its contents are
retained across presentation-only repeats of the same source frame and replaced
on the next source frame, preventing tiny stars or dots from blinking at the
source/presentation beat frequency.

`-waitvsync` remains the backend's swap-chain synchronization control.  It is
independent of the presentation timer and may limit unthrottled speed to the
monitor refresh rate.

## MVEC capture and playback

Use `-vector_record <file.mvec>` to record final vector beam events and
`-vector_playback <file.mvec>` to play them back.  The options are mutually
exclusive.  When recording, VecBeamMAME also writes companion audio to
`<file.mvec>.wav`.

MVEC format 1.1 stores the source vector frame period in the file header.
Playback automatically configures the vector screen, playback clock, audio
synchronization, and frame-domain display effects from that recorded period.
High-refresh repeats of one MVEC frame have zero display-time advance, so
phosphor decay and flicker advance once per recorded frame rather than once per
host present.

Format 1.0 files remain supported.  For timed vector streams, VecBeamMAME
estimates their source rate from the absolute beam-event timestamps.  If a
legacy file has no usable timing information, playback falls back to the
current vector screen rate and reports the fallback in the log.

The bundled [MVEC viewer](../../../tools/mvec-viewer/) opens in a browser and
visualises a recorded beam event stream.
