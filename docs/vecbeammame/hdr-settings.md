# HDR settings

日本語: [hdr-settings.ja.md](hdr-settings.ja.md)

How VecBeamMAME's HDR output works, and how to set it up for a monitor's peak
brightness.  A vector CRT draws thin, very bright lines on black, which suits
HDR well: the background stays black while the beam alone uses the display's
peak.

See also: [Added parameters](added-parameters.md) (the HDR / SDR presentation
section) and [Startup options](startup-options.md).

---

## 1. How it works

The HDR path composites the frame into a nits-referred working buffer and
converts it in the final present pass according to the display mode.  Windows
HDR10 and a macOS EDR with a numeric peak are calibrated in absolute luminance;
macOS EDR `auto` treats the same numbers as a scale relative to SDR reference
white.

Windows HDR10's ST.2084 PQ and SDR's gamma OETF are applied per RGB component.
The aim is that the linear RGB ratios *after* the display's inverse transfer
match across Win HDR, Mac EDR and SDR - not the encoded code-value ratios.
Hue preservation and highlight compression happen in the linear domain, before
the OETF.

- **Windows HDR10** — Rec.2020 primaries, ST.2084 (PQ) transfer, HDR10
  swapchain.  Requires **Windows HDR mode enabled and a d3d11/d3d12 backend**.
  Content outputs absolute nits and the panel tone-maps to its own peak.
- **macOS EDR** — extended linear output.  `1.0` is the display's current SDR
  reference white; a beam above that uses the HDR headroom.  What NSScreen
  returns is this ratio, not absolute nits.  Only when
  `bgfx_hdr_display_peak` is given a number is the physical nit value of
  EDR 1.0 derived, as `specified peak / current headroom`, so the absolute
  working buffer converts correctly to EDR values.
- **SDR fallback** — with HDR unavailable, the same content is tone-mapped for
  an ordinary back buffer.

Writes into the working buffer:

- The vector image (`screen_hdr`) is written at `screen_hdr x beam_peak_nits`
  nits, so **one full-intensity line = `beam_peak_nits` nits**.
- UI, artwork and background are written at `paper_white` nits.

---

## 2. Parameters

| Parameter | Kind | Default | Meaning (in nits) |
|---|---|---|---|
| `bgfx_hdr` | startup option | 1 | Try HDR10/EDR.  Falls back safely to SDR when unavailable.  0 forces SDR. |
| `bgfx_hdr_paper_white` | startup option | 200 | Target white for UI, artwork and background.  On Windows HDR it is replaced automatically when the OS SDR white level can be read.  In macOS relative auto it is the internal reference unit. |
| `bgfx_hdr_display_peak` | startup option | `auto` | Windows auto takes DXGI's absolute peak.  macOS auto calibrates relatively from the headroom ratio alone and does not guess an absolute peak.  To calibrate an XDR in absolute nits, give a number such as `1600`. |
| `beam_peak_nits` | chain slider | 800 (JSON default) | The brightness reference for **one full-intensity vector line**.  Nits under absolute calibration; a relative scale under macOS EDR auto.  Replaced by the derived value when HDR auto is active.  The most important knob. |
| `hdr_rolloff_knee` | chain slider (x beam_peak) | 1.0 | Untouched up to this point (x beam_peak). |
| `hdr_rolloff_max` | chain slider (x beam_peak) | 2.4 (JSON default) | The ceiling that **overlapping lines and overload** approach, asymptotically and hue-preserving, at `beam_peak x max` nits.  Derived from the display peak under HDR auto. |
| `hdr_sat_protect` | chain slider | 0.5 | Protects a saturated colour from blowing out when a single primary alone demands more than peak (colour chain only). |
| `phosphor_gamut` | chain slider | 0.0 | Pulls colour toward real P22 phosphor primaries inside the Rec.2020 container. |
| `edr_sdr_level` | chain slider | 1.0 | Darkens UI and artwork under macOS EDR without touching the HDR beam. |
| `hdr_diagnostics` | chain slider | 0 | Measured overlay of the final HDR/EDR linear buffer.  Normally 0; 1 while tuning. |

> `bgfx_hdr`, `bgfx_hdr_paper_white` and `bgfx_hdr_display_peak` are set on the
> command line or in the ini; the rest are in the slider menu (or in cfg).

The chain defaults in the table are the merged `vector-color`.  The non-HDR
parameters tuned on Star Wars are already in that chain, but `beam_peak_nits`
and `hdr_rolloff_max` depend on the monitor, so no fixed calibration is baked
in - HDR auto handles them.  In SDR an ordinary line starts from
`sdr_beam_level x paper_white` and ends up inside `sdr_rolloff_ceiling`.

---

## 3. How to think about the adjustment

1. **`beam_peak_nits`** is how bright one line is.  Absolute nits on Windows
   HDR10 and on a macOS EDR calibrated with a numeric peak; a unit relative to
   SDR reference white under Mac auto.
2. **`beam_peak_nits x hdr_rolloff_max`** is the ceiling overlapping lines and
   overload approach.  Set it to the monitor's peak nits under absolute
   calibration, or to the available headroom under Mac auto.  Then the shader's
   **hue-preserving roll-off** makes a smooth shoulder before the panel hard
   clips (which shifts colour).
3. Where the absolute peak is known, auto puts one line at **500 nits** if there
   is headroom for it.  Only when 500 nits will not fit inside 85% of the
   display peak does it fall back to the older
   `min(1.65 x SDR reference white, 0.85 x display peak)`.  macOS EDR auto uses
   the relative formula, because the absolute peak is unknown.
4. **`paper_white`** is the white of UI and background.  On Windows HDR the
   "SDR content brightness" setting is read through Win32 DisplayConfig and used
   automatically.  `gun_saturation` is a non-linear per-channel saturation and
   is a separate thing from this white-level calibration.
5. Lowering `hdr_rolloff_knee` to around 0.8 starts the compression before one
   full line, which makes overlap grow more gently.  A matter of taste.

---

## 3.5 Automatic configuration (`bgfx_hdr_display_peak`)

This fork defaults to `-video bgfx -bgfx_hdr 1 -bgfx_hdr_display_peak auto`.  On
Windows the target monitor's Advanced Color state is checked before BGFX is
initialised, and if HDR is on with D3D11/D3D12 (including `auto`), the very
first swapchain is created as HDR10/RGB10A2.  That avoids Windows Auto HDR
noticing a temporary SDR swapchain.  On a monitor or backend without HDR it
falls back to SDR, and `-bgfx_hdr 0` forces SDR explicitly.

- Windows peak: DXGI `IDXGIOutput6::GetDesc1().MaxLuminance`
- Windows SDR white:
  `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)`,
  applied automatically as the HDR10 paper white for UI and artwork
- macOS `auto`: `maximumPotentialExtendedDynamicRangeColorComponentValue` is
  used only to decide whether EDR is supported;
  `maximumExtendedDynamicRangeColorComponentValue` is read per frame as the
  currently available EDR headroom.  Neither is converted to absolute peak nits
- macOS with a number: `EDR reference white nits = specified display peak /
  current headroom`, and present outputs `internal nits / EDR reference white
  nits`.  For an XDR at 1600 nits with headroom 16, EDR 1.0 is 100 nits
- Ordinary line (absolute peak known): 500 nits when
  `500 <= 0.85 x display peak`.  Without the headroom it falls back to
  `min(1.65 x SDR white, 0.85 x display peak)`, rounded to 10 nits
- Ordinary line (macOS auto): no absolute peak is guessed; the relative value
  equivalent to 1.65 x SDR reference white, as before
- Overload ceiling: Windows and numeric peaks use
  `hdr_rolloff_max = display peak / beam_peak` (clamped 1.1 to 8.0).  macOS auto
  keeps the chain default, cfg or manual value and limits only the present-time
  effective ceiling to `min(user ceiling, current EDR headroom)`
- If detection fails: the chain default is kept.  1000 nits is not assumed
- Priority: chain default < derived automatically < saved manual cfg value <
  a slider moved while running

When a session ends still on the derived values, `beam_peak_nits` and
`hdr_rolloff_max` are not written to cfg, so the next launch - or a different
monitor - can derive them again from that display.  Move the slider by even one
step and it is saved as an ordinary cfg value, which then takes priority over
auto.

`-verbose` reports the active path, SDR white, headroom and the derived values.
macOS auto shows the potential capability and the current headroom once the EDR
layer is active.  With a numeric peak it shows the specified peak, the headroom
and the derived EDR reference white.

### 3.6 HDR diagnostics overlay

Setting the **HDR Diagnostics Overlay** slider to 1 reads back the linear nits
buffer just before the final present, about every 30 frames, and displays the
following at the top left.

- Windows HDR10: the absolute display peak from the OS, the headroom against
  paper white, and nit values before and after roll-off
- macOS EDR with a number: the specified display peak, the EDR reference white
  derived as `peak / headroom`, and absolute nits before and after roll-off
- macOS EDR auto: states `Absolute display peak: unknown` and shows beam, pre,
  post, top-average and whole-screen-average as EDR multiples (`x`)
- Under absolute calibration it also shows the average of the top 128 pixels and
  the count of pixels above 80% of display peak and above 1000 nits

The values come from the linear buffer just before the final present.  On
Windows HDR10 and macOS EDR with a numeric peak they are the requested absolute
nits; under Mac auto they are multiples of SDR reference white.  They are not a
measurement of what the panel actually emits.  Use them to see how close the
post-roll-off peak comes to the display peak or headroom, and whether
overlapping draws are using the room available.  Under SDR fallback nothing is
measured and the overlay reports that HDR/EDR output is inactive.  It performs a
full-screen GPU readback, so set it back to 0 for performance measurement and
for ordinary play.

## 4. Examples by monitor

| Display | Mode | EDR/SDR reference | Peak | beam | rolloff max |
|---|---|---:|---:|---:|---:|
| Windows HDR400 | auto | with SDR white at 200 nits | 400 nits | 330 nits | 1.21 |
| Windows (measured example) | auto | SDR white 148 nits | 604 nits | 500 nits | 1.21 |
| Windows (high peak example) | auto | SDR white 240 nits | 1390 nits | 500 nits | 2.78 |
| macOS XDR | auto (relative) | current headroom 16x / nominal white 200 | unknown | 1.65x (nominal 330) | chain/cfg value (limited dynamically by current headroom) |
| macOS XDR | `bgfx_hdr_display_peak 1600` | EDR reference white 100 nits (at headroom 16x) | 1600 nits | 500 nits | 3.20 |

To make an XDR agree with Windows HDR in absolute luminance, specify
`bgfx_hdr_display_peak 1600`.  After that the diagnostics overlay should read
`macOS EDR, absolute` and show something like `Reference white: 100.0 nits`.
Headroom changes with screen brightness, power and temperature, so it is
tracked per frame while running.

On a display with enough absolute peak, an ordinary line sits at 500 nits and
the remaining peak capability goes to overlapping lines, overload and
explosions.  On a low-peak display it returns to the older paper-white formula.
A Beam Peak or Highlight Max set by hand in a saved cfg takes priority over the
automatic value, so reset those cfg entries when you want to see what auto
alone produces.

## 5. Notes on macOS EDR

- EDR `1.0` is not a fixed 100 or 200 nits: it is the current SDR reference
  white (the UI white).  `NSScreen.maximumExtendedDynamicRangeColorComponent
  Value` is the ratio "HDR peak / current SDR reference white", and on its own
  it cannot determine the absolute peak in nits.
- `bgfx_hdr_display_peak auto` is a safe **relative mode**, outputting
  `L / bgfx_hdr_paper_white`.  Its diagnostics are EDR multiples rather than
  nits, and no physical peak is guessed.
- With a numeric peak it becomes an **absolute mode**: it derives
  `EDR reference white = specified peak / headroom` and outputs
  `L / EDR reference white`.  There, `beam_peak_nits` and the diagnostics
  overlay can be read as absolute nits.
- On macOS versions where the current headroom returns 1.0 until the EDR layer
  is active, the potential headroom is used for the capability test only, to
  avoid a circular decision.  The current value is not trusted until it exceeds
  1.0 after the layer is displayed, and is read per frame after that.
- A drop in current headroom is applied immediately, to prevent clipping; a rise
  is smoothed over about a second.  Ordinary lines, `hdr_rolloff_max` and cfg
  are not rewritten - only the present shader's effective ceiling is limited.
- EDR uses 709 primaries, which are sRGB primaries, so `phosphor_gamut` (meant
  for the Rec.2020 container) is skipped on the EDR path.

### 5.1 Diagnostic options for direct EDR display

If, on an external HDR monitor in full screen, the background and ordinary beam
alone go dark, check the layer state and present mode with the diagnostic log
and the Metal HUD.

- `-bgfx_macos_edr_diagnostics`: logs the CAMetalLayer address, screen, pixel
  format, EDR/opaque/transaction attributes, colorspace, EDR metadata, contents
  scale and raw current headroom once a second.
- `bgfx_macos_force_composited` is enabled by default.  It makes the
  CAMetalLayer non-opaque to request composited display, but does not change
  `presentsWithTransaction`, so bgfx's asynchronous present is not stalled.
  Confirm in the Metal HUD that present mode has become `Composited`.
- For an A/B against the direct-to-display path, start with
  `-nobgfx_macos_force_composited`.  If it is stable only with the default, the
  macOS direct EDR path is the likely cause.

---

## 6. Notes

- The values here are **starting points**.  How it actually looks depends on the
  panel's tone-mapping, so settle the final impression of brightness on the
  display itself.
- Values tuned for a monitor can be saved in cfg (per game, too), or baked into
  the chain defaults as a fixed preset.
- `beam_peak_nits` is "the absolute brightness of one line", independent of
  `beam_width_*`, `intensity_overdrive` and the rest (line width and core
  emphasis).  Brightness with `beam_peak_nits`, width with the width controls,
  how much it blows out with the roll-off: that division works well.
- `vector-color`'s 1/64 wide-glow reach does not change the nit calibration.  It
  redistributes light into the far halo as a visual effect; the reference nits
  of an ordinary line are still set by `beam_peak_nits`.
