# VecBeamMAME balanced vector chains

The `balanced` chains are the recommended VecBeamMAME vector display
configurations:

- `vector-color-balanced`
- `vector-color-balanced-portrait`
- `vector-monochrome-balanced_1`
- `vector-monochrome-balanced_2`
- `vector-vectrex-balanced`

Select the appropriate chain from MAME's video options or with
`-video bgfx -bgfx_screen_chains <chain-name>`.  BGFX slider values are saved
per system in the MAME configuration files.

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
