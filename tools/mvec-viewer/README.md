# MVEC Viewer

Open `mvec-viewer.html` in Chrome, Edge or Safari and pick a file recorded by MAME's
`-vector_record`. Everything is processed inside the browser; nothing is sent anywhere.

The tool's user interface is English only. A Japanese copy of this document is in
[README.ja.md](README.ja.md).

## Frame numbering

- `Data X / N` is the 1-based position of the frame counted from the start of the MVEC.
- Typing a number into `Data frame` jumps straight to that position.
- `Recorded frame ID` is the 0-based ID stored in the MVEC itself.
- When comparing notes with someone else, quote the X of `Data X / N`, e.g. "data frame 125".

## Layout

The frame controls (transport, `Data frame`, frame position slider) sit directly under the
file-info line. The display is below them.

## Controls

- `Prev` / `Next`: step one frame
- `|<` / `>|`: jump to the first / last frame
- `Play`: play back at 60 frames/second
- Left / right arrow keys: previous / next frame
- Space: play / stop
- Frame position slider: seek anywhere
- Orientation: rotate the recorded coordinates in 90° steps
- Zoom: mouse wheel (zooms about the cursor) / `＋` `−` buttons / `+` `-` keys
- Pan: drag the canvas while zoomed in. This works in reference mode too.
- Back to the whole frame: `Fit` button / double-click the canvas / `0` key
- Show blank moves: also draw the beam moves at intensity 0
- Click a vector: shows RGB, intensity, beam energy, draw time and more
- Reference mode: puts the display and the data table side by side, draws only the vectors up
  to the selected row, and highlights that row in yellow
- Click a table row / `Prev row`, `Next row` / `Vector row` / the slider: select a vector row
- Up / down arrow keys in reference mode: previous / next vector row

Every frame's CRC32 is verified at load time. Stale frames show the last valid vector list,
exactly as MAME does during playback.

## Reference-mode table

`Rows shown` sets how many rows the table draws at once — 10, 15 (default), 25, 50, 100, 250
or 1000. The window is centred on the selected row, and the row-window line above the table
says which range is on screen. Only that window is put into the DOM, so a frame with 20000
vectors stays responsive; navigate with the row controls or the slider to reach the rest.

The table scrolls in both directions: horizontally because the columns keep their natural
width instead of being squeezed, and vertically once the chosen row count exceeds 70% of the
window height. The header row stays pinned while you scroll.

Alongside the recorded fields, three columns are computed from the record:

| Column | Meaning |
| --- | --- |
| `len` | Segment length in screen units: `hypot(x1-x0, y1-y0) / 65536`. Coordinates are stored in 1/65536 screen units on both axes, so lengths are directly comparable with the `vis` extents of the frame. |
| `dt µs` | Draw time of the vector, `t1 - t0`, in microseconds. |
| `dt %` | The vector's share of the frame's total draw time. |
| `µs/len` | Draw time per unit of length, `dt / len`. Effectively the inverse of beam speed, so it is nearly constant for ordinary line draws and stands out where it is not. |

The row-window line also reports the frame's total draw time — the sum of `dt µs` over every
vector, blank moves included, since the beam spends that time too.

`t0`/`t1` are only meaningful for frames the recorder timed. For an untimed frame all four
of these columns show `—`; `len` is still computed.

## Supported formats

MVEC **1.0 / 1.1** can be read (`MVEC_VERSION_MAJOR/MINOR` in `vector.cpp`). 1.1 only adds the
**recorded frame period (attoseconds, i64)** after the header's system / device strings; the
frame chunks and the 66-byte vector records are identical to 1.0. When the period is present
the status line shows the recording rate, e.g. `1.1 ... 60.000Hz` (1.0 has no period, so
nothing is shown). Files with a minor version of 2 or above are rejected outright.

This viewer is for inspecting primitive vector data with additive blending. It does not
reproduce the BGFX chain's afterglow, glow, shadow mask, HDR/EDR or anything similar.
