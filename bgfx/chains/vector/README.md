# VecBeamMAME balanced vector chains

The `balanced` chains are the recommended VecBeamMAME vector display
configurations:

- `vector-color-balanced`
- `vector-monochrome-balanced_1`
- `vector-monochrome-balanced_2`
- `vector-vectrex-balanced`

Select the appropriate chain from MAME's video options or with
`-video bgfx -bgfx_screen_chains <chain-name>`.  BGFX slider values are saved
per system in the MAME configuration files.

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

## Smoked-glass optics

The final linear HDR composite includes an optional uniform smoked-glass model.
It is evaluated before HDR/SDR presentation and is separate from MAME artwork
overlays.  It does not use a dirt, scratch, or noise texture.

### Controls

`Smoked Glass RGB (%)`
: Glass transmission colour, specified as RGB percentages from 0 to 100.

`Smoked Glass Transmission (%)`
: Interpolates from the configured glass colour at 0% to clear glass at 100%.
  The effective per-channel filter is:

  `mix(smoked_glass_rgb / 100, 1, transmission / 100)`

`Glass Forward Scatter (%)`
: Adds a broad transmitted halo while preserving the original direct image.
  Its range is 0.0% to 10.0% in 0.1% steps.  The added halo passes through the
  smoked-glass colour filter.

`Glass Surface Illumination (%)`
: Adds broad light returned from the viewer-facing glass surface.  Its range is
  0.0% to 10.0% in 0.1% steps.  This component is added after the bulk
  transmission filter.

In simplified form:

```text
filter      = mix(glassRGB, 1, transmission)
transmitted = (directComposite + broadHalo * forwardScatter) * filter
surfaceLit  = broadHalo * surfaceIllumination
output      = transmitted + surfaceLit
```

With transmission at 100% and both optical controls at 0%, the glass operation
is an exact identity.  Clear glass can still show scattering if either optical
control is non-zero.

The broad halo reuses the existing 1/16-resolution glow cascade.  No additional
full-screen blur or composite pass is introduced.  If `Glow Wide` and both
glass optical controls are all zero, the related downsample cascade can be
skipped.

## Starting point for smoked cabinet glass

The following values are a conservative starting point for a neutral grey
cabinet overlay such as the one in *Star Wars*:

```text
Smoked Glass RGB (%)             62 / 62 / 62
Smoked Glass Transmission (%)    50.0
Glass Forward Scatter (%)         1.0
Glass Surface Illumination (%)    0.5
```

Increase forward scatter to spread bright vectors through the glass without
softening or replacing the direct lines.  Increase surface illumination when
bright halos should appear to light the glass itself.
