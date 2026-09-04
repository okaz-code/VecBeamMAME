# MAME Vector Primary Colour Tuner

Open `index.html` in a browser and it works. No network connection, no install.

The tool's user interface is English only. A Japanese copy of this document is in
[README.ja.md](README.ja.md).

- Tunes red, green and blue individually by Hue / Saturation / Brightness, for Direct Primary mode.
- The achromatic component is kept as white, so tuning the RGB primaries does not tint a white dot.
- To carry a setting into MAME, use the chain JSON on the right.
- `Reset (RGB reference)` returns every colour to Hue Shift 0°, Saturation 1.0, Brightness 1.0. It does
  not read the chain's current defaults.
- Like MAME's fixed SDR path, the preview applies the hue-preserving linear roll-off first and the
  per-channel RGB gamma second.
- The HEX values and the canvas are a guide to hue and saturation for an SDR display. They do not
  reproduce real HDR/EDR luminance or a display's own tone mapping, so check Brightness above 1.0 and
  any high-luminance colour on real hardware.
- Raising Saturation above 1.0 clips the negative components to 0, so a strong setting can shift hue
  and not only saturation.
- The older CIE xy/Y mode is still there as an Advanced choice on the chain side.
