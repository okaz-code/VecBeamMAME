# HDR settings

日本語: [hdr-settings.ja.md](hdr-settings.ja.md)

How VecBeamMAME's HDR output works, and how it is calibrated.  A vector CRT
draws thin, very bright lines on black, which suits HDR well: the background
stays black while the beam alone uses the display's peak.

See also: [Added parameters](added-parameters.md) (the HDR / SDR presentation
section) and [Startup options](startup-options.md).

---

## 1. How it works

The HDR path composites the frame into a working buffer measured in **nits**,
and the final present pass encodes it for the output in use.

ST.2084 PQ on Windows HDR10 and the SDR gamma OETF are applied per RGB
component.  What has to match across Win HDR, Mac EDR and SDR is the linear RGB
ratio the display reconstructs, not the ratio of the encoded code values.
Hue-preserving highlight compression happens before the OETF, in linear light.

- **Windows HDR10** — Rec.2020 primaries, ST.2084 (PQ), HDR10 swapchain.
  Requires Windows HDR mode on and a d3d11/d3d12 backend.  The content emits
  absolute nits and the panel tone-maps to its own peak.
- **macOS EDR** — extended-linear output.  `1.0` is the display's current SDR
  reference white and anything above it uses the HDR headroom.  NSScreen
  reports that ratio and never absolute nits, but **the headroom is reported
  against a fixed 100-nit reference**, which is enough to recover absolute nits
  (§3.5).
- **SDR fallback** — the same image tone-mapped for an ordinary backbuffer.
  Absolute nits are undefined there, so SDR keeps its own normalised set
  (`sdr_*`).

What lands in the working buffer:

- the vector image at **`beam_nits`** (derived from the calibration, §3), so one
  full-intensity line is `beam_nits`
- UI, artwork and background at the **reference white** — the current SDR white
  on EDR, the OS-reported SDR white on Windows HDR10

---

## 2. Parameters

### Calibration (shared across platforms, absolute nits)

| Parameter | Default | Meaning |
|---|---:|---|
| `hdr_peak_target_nits` | 3000 | The **brightest thing on screen** - a spoke, an enemy bullet, an explosion centre.  Clamped by the display's peak |
| `hdr_beam_target_nits` | 375 | One ordinary vector |
| `hdr_beam_floor_nits` | 250 | The ordinary beam is not allowed below this |

**These three are the calibration.**  All absolute, none platform- or
monitor-dependent, so the same values go in the Mac cfg and the Windows cfg.

### Display-side shaping

| Parameter | Default | Meaning |
|---|---:|---|
| `hdr_shoulder_start` | 0.85 | Where the shoulder begins, as a **fraction of the display ceiling**.  Everything below it is linear |
| `hdr_sat_protect` | 0.0 | Guards a saturated colour that asks one primary for more light than it has (colour chains only) |
| `hdr_glow_stability` | 1.0 | Holds beam-derived glow at constant absolute nits when the beam peak moves |
| `hdr_headroom_override` | 0.0 | Pins the present ceiling (x SDR white).  0 = follow the display.  **Use it to make calibration reproducible** |

### Derived (do not set)

| Parameter | Content |
|---|---|
| `beam_peak_ratio` | `beam_nits / reference white`.  Lands on a different number per platform, by design |
| `hdr_rolloff_max` | `ceiling / beam`.  Falls below the target ratio when the display cannot reach the target |

While `hdr_peak_target_nits > 0` these two are **withheld from the slider
menu**.  They keep their values and are still saved; nothing reads them.

### SDR path (separate, because absolute nits are undefined there)

| Parameter | Default | Meaning |
|---|---:|---|
| `sdr_beam_level` | 0.90 (mono 0.72) | Output level of the ordinary beam (x paper white) |
| `sdr_rolloff_knee` | 0.75 | SDR shoulder start (x white) |
| `sdr_rolloff_ceiling` | 1.00 | SDR ceiling (x white) |
| `sdr_shadow_curve` | 0.95 | Midtone reshape |
| `bright_normal_cap` | 0.8 (mono 0.6) | Ceiling on ordinary beams, released only by n > 1 overdrive |

### Startup options

| Option | Default | Meaning |
|---|---|---|
| `bgfx_hdr` | 1 | Attempt HDR10/EDR, fall back to SDR.  0 = force SDR |
| `bgfx_hdr_display_peak` | `auto` | Display peak in nits.  `auto` / a number / `0` (derive nothing, keep chain defaults) |
| `bgfx_macos_edr_reference_white` | 100 | **The nits an EDR headroom of 1.0 stands for.**  Panel peak = potential headroom x this |
| `bgfx_macos_edr_calibration` | `absolute` | Calibration basis.  `absolute` = the targets are absolute nits and hold across brightness changes; `relative` = read against a nominal SDR white and follow the slider (§3.7) |
| `bgfx_hdr_paper_white` | 200 | UI white in nits.  **Effectively inert**: overridden on both HDR paths and cancelled out in SDR (§6) |

> Startup options go on the command line or in an ini; everything else is a
> slider (or a cfg entry).

---

## 3. The calibration model

What has to be calibrated is **absolute luminance**, not a ratio to SDR white.
SDR white is the OS "SDR content brightness" setting on Windows and the 100-nit
convention on macOS, so **the same ratio means a different amount of light on
each**: measured on one monitor, 2.5 x SDR white came out at 600 nits under
Windows and about 250 on macOS.

```
ceiling = min(hdr_peak_target_nits, panel peak)
beam    = clamp(ceiling * (hdr_beam_target_nits / hdr_peak_target_nits),
                hdr_beam_floor_nits, hdr_beam_target_nits)
```

The target is the **peak** because that is what a real tube is judged by, and
the ordinary beam sits at a fixed fraction under it.  A brighter display then
raises both together, so **the picture keeps its ratios and simply gets
brighter**; a 3000-nit monitor reaches the target and nothing changes above
that.

The **floor** stops a panel short of the target from dragging the ordinary beam
down with it.  There the overrange takes the loss instead - a correctly bright
picture with squashed highlights beats a dim one with perfect ratios.

The derivation runs at the point of use, every frame, so cfg edits, brightness
changes and display changes all take effect with no bookkeeping.

---

## 3.5 Automatic configuration

The default is `-video bgfx -bgfx_hdr 1 -bgfx_hdr_display_peak auto`.  On
Windows the target monitor's Advanced Color state is checked before BGFX
initialises, and the very first swapchain is created as HDR10/RGB10A2 when HDR
is on with D3D11/D3D12 - this avoids Windows Auto HDR latching onto a temporary
SDR swapchain.

**Where the panel peak comes from:**

| | Source |
|---|---|
| Windows | DXGI `IDXGIOutput6::GetDesc1().MaxLuminance` |
| Windows SDR white | `DisplayConfigGetDeviceInfo(...GET_SDR_WHITE_LEVEL)`, which also becomes the HDR10 UI white |
| macOS `auto` | **potential headroom x `bgfx_macos_edr_reference_white`** (default 100) |
| macOS explicit | the number given |

The 100-nit convention was measured on two displays:

```
built-in Liquid Retina XDR : potential 16.00x -> 1600 nits (spec 1600)
external                   : potential 14.05x -> 1405 nits (DXGI reports 1390; EDID rounding)
```

The current SDR white is `panel peak / current headroom`.  Raising the
brightness slider raises that white and drops the current headroom in step, so
the expression follows brightness on its own.  The result is logged when it
resolves and whenever it moves:

```
BGFX: macOS EDR absolute scale: panel peak 1600 nits (potential headroom x reference white),
      SDR white 100 nits at current 16.00x
```

For a display that does not follow the convention, correct it with
`bgfx_macos_edr_reference_white`, or give the peak directly with
`bgfx_hdr_display_peak` (a number overrides the whole derivation).

The **present ceiling** follows the current headroom: a fall takes effect
immediately to prevent clipping, a rise is smoothed over about a second.  On
macOS that moves with the screen brightness, so **calibrate at a fixed
brightness, or pin the ceiling with `hdr_headroom_override`**.

`-verbose` reports the active path, SDR white, headroom and the derived values.

---

## 3.7 Calibration basis (absolute / relative)

`bgfx_macos_edr_calibration` selects between two bases.

| | `absolute` (default) | `relative` |
|---|---|---|
| What the targets mean | Absolute nits | Multiples of a nominal SDR white (`bgfx_hdr_paper_white`) |
| Panel peak as seen | `potential x 100`, fixed | `current x paper_white`, moves with brightness |
| Raising the brightness | The picture holds, **only the UI brightens** | **Both the picture and the ceiling brighten** |
| Sharing values Mac/Win | Works | Breaks |

`relative` is the behaviour from before the absolute derivation existed, and is
implemented by skipping that derivation.

**So `relative` only does anything with `bgfx_hdr_display_peak auto`.**  A
numeric peak sets `m_hdr_display_peak_absolute` and takes the absolute
derivation branch, which never reads the calibration basis.

Measured on the built-in XDR (potential 16.00x, i.e. a real 1600-nit panel):

| Mode | paper_white | current | Panel as seen | Ceiling | Beam | Emitted |
|---|---:|---:|---:|---:|---:|---:|
| absolute | any | 16.00x | 1600 | 1600 | 250 | **250 nits** |
| absolute | any | 8.00x | 1600 | 1600 | 250 | **250 nits** |
| absolute | any | 4.00x | 1600 | 1600 | 250 | **250 nits** |
| relative | 200 | 16.00x | 3200 | 3000 | 375 | 188 nits |
| relative | 100 | 16.00x | 1600 | 1600 | 250 | **250 nits** |
| relative | 100 | 8.00x | 800 | 800 | 250 | 500 nits |
| relative | 100 | 4.00x | 400 | 400 | 250 | 1000 nits |

The Windows external panel under the same calibration (DXGI 1390, SDR white
240) emits **250 nits**.

So **`relative` agrees with Windows only when `potential == current` and
`paper_white` matches the real reference white of 100** - and under exactly
those conditions `current x paper_white = potential x 100`, so **`relative`
degenerates into `absolute`**.  Left at the default paper white of 200 it is
out by a factor of two.

`absolute` exists for the case where potential and current diverge - which is
to say, whenever the brightness slider moves away from the point the reference
was measured at.  In the table above `absolute` holds 250 nits across current
16 -> 8 -> 4 while `relative` doubles each step, 250 -> 500 -> 1000.

**Use `absolute` if the point is to share values between Mac and Windows.**
`relative` is for the separate goal of wanting the picture to track the
brightness slider, and there `-bgfx_hdr_paper_white 100` is effectively
mandatory alongside it.

There is no third basis that tracks only the ceiling.  Moving the SDR white
point does not change what the panel can emit - `current x current SDR white =
potential x 100 = panel peak`, the terms cancel - so an absolute ceiling has
nothing to track.

---

## 4. Measured examples

The calibration is identical on all three - peak 3000 / beam 375 / floor 250.
Only what it derives differs.

| Display | Panel peak | Ceiling | Beam | Effective ratio | knee / beam |
|---|---:|---:|---:|---:|---:|
| Windows external (DXGI 1390, SDR white 240) | 1390 | 1390 | 250 (floor) | 5.56 | 4.73 |
| macOS external (potential 14.05x) | 1405 | 1405 | 250 (floor) | 5.62 | 4.78 |
| macOS built-in XDR (potential 16.00x) | 1600 | 1600 | 250 (floor) | 6.40 | 5.44 |
| 3000-nit (projected) | 3000 | 3000 | 375 | 8.00 | 6.80 |
| 5000-nit (projected) | 3000 (target met) | 3000 | 375 | 8.00 | 6.80 |

`knee / beam` is the **linear room left above one ordinary vector**.  At 1.0
every structure above one beam - additive overlap, vertex dwell, overdrive -
lands on the shoulder and its gradation is lost.

---

## 5. Notes on macOS EDR

- EDR `1.0` is not a fixed value but the current SDR reference white.
  `maximumExtendedDynamicRangeColorComponentValue` is the ratio "HDR peak over
  current SDR white".
- **Potential and current are different quantities.**  Potential is a panel
  property and stable across brightness, so it derives the **panel peak**;
  current follows brightness, so it drives the **present ceiling**.  Confusing
  the two makes one unchanged configuration land on two different tone scales.
- Before the EDR layer is on screen the current headroom reads 1.0, the
  documented bootstrap value.  The derivation waits rather than importing it.
- Paper white follows the derived reference white, and must: the UI is drawn at
  paper_white nits and the present pass divides by the reference white, so
  leaving them apart renders the UI at `paper_white / reference`.
- EDR uses 709 primaries, i.e. sRGB primaries.

### 5.1 Diagnostic options for direct EDR display

If only the background and ordinary beams look dark in fullscreen on an
external HDR monitor, check the layer state and present mode with the
diagnostics log and the Metal HUD.

- `-bgfx_macos_edr_diagnostics`: logs the CAMetalLayer address, screen, pixel
  format, EDR/opaque/transaction attributes, colorspace, EDR metadata, contents
  scale and raw current headroom once per second.
- `bgfx_macos_force_composited` is on by default.  It makes the CAMetalLayer
  non-opaque to request Composited presentation, and deliberately does not
  touch `presentsWithTransaction`, which would stall bgfx's asynchronous
  present.  Confirm the Metal HUD shows Present mode `Composited`.
- For an A/B against the direct-to-display path, start with
  `-nobgfx_macos_force_composited`.  If only the default is stable, macOS's
  direct EDR path is the cause.

---

## 6. Notes

- The calibration values are a **starting point**.  Panels differ in their own
  tone mapping, so settle the final impression on the actual hardware.
- **`bgfx_hdr_paper_white` is effectively inert.**  Windows HDR10 overrides it
  with the OS SDR white and macOS EDR with the derived reference white; in SDR
  `seed_peak = paper_white * sdr_beam_level` is divided by `paper_white` at
  present, so it cancels.  It remains for compatibility and is not a
  calibration control.
- Brightness comes from the three calibration values, thickness from
  `beam_width_*`, and how highlights blow out from the shoulder.
- `beam_peak_nits`, `hdr_rolloff_knee`, `hdr_diagnostics`, `phosphor_gamut` and
  `edr_sdr_level` are retired.  `beam_peak_nits` survives in code only, as the
  fallback for a chain that does not carry the three calibration values.
