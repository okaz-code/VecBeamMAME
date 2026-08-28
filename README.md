# VecBeamMAME

**日本語の説明は [README.ja.md](README.ja.md) にあります。**

VecBeamMAME is an unofficial, experimental fork of MAME aimed at a physically
motivated, HDR-aware simulation of vector-scan displays.  Instead of drawing a
vector primitive from `X / Y / Z / Colour` alone, it carries beam timing and
beam metadata through the renderer, and approximates how a real vector CRT
responds to beam movement, beam energy, phosphor excitation, redraw, focus and
optical scatter.

It builds as `vbmame` and reads `vbmame.ini`, so it can sit alongside a stock
MAME installation.

## What this is

A vector primitive normally reaches a renderer as little more than `X / Y / Z
(intensity) / colour`.  VecBeamMAME carries **beam timing and beam metadata**
alongside it, and approximates how a real vector CRT responds to beam movement,
beam energy, phosphor excitation, redraw, focus and optical scatter.

Rather than simply drawing lines, it aims to reproduce differences like these:

- A short line drawn slowly is brighter than a long line drawn fast.
- Redrawing the same place before the phosphor has decayed is brighter than a
  single pass (accumulation).
- A high-energy vector is wider and less sharply focused.
- A parked beam (a dwell dot) and beam excursions off the visible area both
  contribute light.

## The three-layer model

The reproduction is deliberately split into three layers, to keep clear what is
*emulation* (faithful), what is *simulation* (physical approximation) and what
is *empirical tuning* (matching an appearance).

1. **Emulation of the vector generator** — the timing and signals the hardware
   produced: AVG/DVG/Vectrex vector timing, start and end times, draw time,
   visible time, blanked travel time, Z/intensity timing, scale-dependent travel
   time, known overload conditions.  This is a record of what the original
   circuit put out, and belongs on the faithful side.
2. **Simulation of the vector CRT** — how those signals look on a real display,
   approximated from physical principles: beam speed to energy, phosphor
   persistence and accumulation, focus and defocus, the appearance of overload,
   glow and halo, off-screen excursions.
3. **Optical rendering and empirical tuning** — the final look, which is hard to
   measure per machine.  **It is matched against the real monitors**, side by
   side.  Photographs and video are consulted only loosely, because a camera
   greatly exaggerates a CRT's highlights.

> Calling the whole display path "complete hardware emulation" would be wrong.
> So would dismissing it as "just a bloom shader", since that ignores the beam
> timing and metadata it adds.

## Documentation

| | |
|---|---|
| [Added parameters](docs/vecbeammame/added-parameters.md) | The runtime sliders, generated from `bgfx/chains/vector/*.json` |
| [Startup options](docs/vecbeammame/startup-options.md) | What goes in `vbmame.ini`, starting with how to create one |
| [HDR settings](docs/vecbeammame/hdr-settings.md) | How the HDR path works and how to match it to a display |
| [FAQ](docs/vecbeammame/faq.md) | Frequently asked questions |
| [Vector chain guide](bgfx/chains/vector/README.md) | The bundled BGFX chains |

Bundled tools: [MVEC viewer](tools/mvec-viewer/) (beam event stream inspection)
and [colour tuner](tools/vector-color-tuner/) (primary adjustment).  Both are
self-contained HTML - open them in a browser.

---

## Support

**Please do not ask the upstream MAME developers about anything specific to
VecBeamMAME.**  They do not maintain this fork, cannot help with it, and a
fork's bug reports are noise on their tracker.

Use this repository's Issues instead.  How the picture looks depends heavily on
the display, so include your OS, GPU, renderer, display type, whether HDR is
on, the game, and the command line you used.

If the same thing happens on stock MAME 0.289, it belongs upstream rather than
here.

---

## Upstream MAME

## What is MAME?

MAME is a multi-purpose emulation framework.

MAME's purpose is to preserve decades of software history. As electronic technology continues to rush forward, MAME prevents this important "vintage" software from being lost and forgotten. This is achieved by documenting the hardware and how it functions. The source code to MAME serves as this documentation. The fact that the software is usable serves primarily to validate the accuracy of the documentation (how else can you prove that you have recreated the hardware faithfully?). Over time, MAME (originally stood for Multiple Arcade Machine Emulator) absorbed the sister-project MESS (Multi Emulator Super System), so MAME now documents a wide variety of (mostly vintage) computers, video game consoles and calculators, in addition to the arcade video games that were its initial focus.

## Where can I find out more?

* [Official MAME Development Team Site](https://www.mamedev.org/) (includes binary downloads, wiki, forums, and more)
* [MAME Testers](https://mametesters.org/) (official bug tracker for MAME)

### Community

* [MAME Forums on bannister.org](https://forums.bannister.org/ubbthreads.php?ubb=cfrm&c=5)
* [r/MAME](https://www.reddit.com/r/MAME/) on Reddit
* [MAMEWorld Forums](https://www.mameworld.info/ubbthreads/)

## Development

![Alt](https://repobeats.axiom.co/api/embed/8461d8ae4630322dafc736fc25782de214b49630.svg "Repobeats analytics image")

### CI status and code scanning

[![CI (Linux)](https://github.com/mamedev/mame/workflows/CI%20(Linux)/badge.svg)](https://github.com/mamedev/mame/actions/workflows/ci-linux.yml) [![CI (Windows](https://github.com/mamedev/mame/workflows/CI%20(Windows)/badge.svg)](https://github.com/mamedev/mame/actions/workflows/ci-windows.yml) [![CI (macOS)](https://github.com/mamedev/mame/workflows/CI%20(macOS)/badge.svg)](https://github.com/mamedev/mame/actions/workflows/ci-macos.yml) [![Compile UI translations](https://github.com/mamedev/mame/workflows/Compile%20UI%20translations/badge.svg)](https://github.com/mamedev/mame/actions/workflows/language.yml) [![Build documentation](https://github.com/mamedev/mame/workflows/Build%20documentation/badge.svg)](https://github.com/mamedev/mame/actions/workflows/docs.yml)  [![Coverity Scan Status](https://scan.coverity.com/projects/5727/badge.svg?flat=1)](https://scan.coverity.com/projects/mame-emulator)

### How to compile?

If you're on a UNIX-like system (including Linux and macOS), it could be as easy as typing

```
make
```

for a full build,

```
make SUBTARGET=tiny
```

for a build including a small subset of supported systems.

See the [Compiling MAME](http://docs.mamedev.org/initialsetup/compilingmame.html) page on our documentation site for more information, including prerequisites for macOS and popular Linux distributions.

For recent versions of macOS you need to install [Xcode](https://developer.apple.com/xcode/) including command-line tools and [SDL 2.0](https://github.com/libsdl-org/SDL/releases/latest).

For Windows users, we provide a ready-made [build environment](http://www.mamedev.org/tools/) based on MinGW-w64.

Visual Studio builds are also possible, but you still need [build environment](http://www.mamedev.org/tools/) based on MinGW-w64.
In order to generate solution and project files just run:

```
make vs2022
```
or use this command to build it directly using msbuild

```
make vs2022 MSBUILD=1
```

### Coding standard

MAME source code should be viewed and edited with your editor set to use four spaces per tab. Tabs are used for initial indentation of lines, with one tab used per indentation level. Spaces are used for other alignment within a line.

Some parts of the code follow [Allman style](https://en.wikipedia.org/wiki/Indent_style#Allman_style); some parts of the code follow [K&R style](https://en.wikipedia.org/wiki/Indent_style#K.26R_style) -- mostly depending on who wrote the original version. **Above all else, be consistent with what you modify, and keep whitespace changes to a minimum when modifying existing source.** For new code, the majority tends to prefer Allman style, so if you don't care much, use that.

All contributors need to either add a standard header for license info (on new files) or inform us of their wishes regarding which of the following licenses they would like their code to be made available under: the [BSD-3-Clause](http://opensource.org/licenses/BSD-3-Clause) license, the [LGPL-2.1](http://opensource.org/licenses/LGPL-2.1), or the [GPL-2.0](http://opensource.org/licenses/GPL-2.0).

See more specific [C++ Coding Guidelines](https://docs.mamedev.org/contributing/cxx.html) on our documentation web site.

## Use of AI

This fork's code and documentation were written with AI coding agents.  It is
not something to hide, so it is stated here.

**The ideas and the judgement are human; the code and the tests are the AI's.**

- **Human** — what to build, what the physical model should be, how it compares
  against real hardware (Amplifone, WG6100, a Vectrex), and whether to keep it
  or throw it away
- **AI** — the code that implements it, the measurements and scripts that check
  it, and the writing

Whether a model is right is settled by looking at real hardware, not by taking
the AI's explanation at face value.  Features have been built, looked at, and
removed again.

Commits with AI involvement carry a `Co-Authored-By:` trailer, so `git log`
shows the extent of it.  Responsibility for the code and the documentation
rests with this repository's author.

Upstreaming to MAME is not intended.  VecBeamMAME is an experimental fork; a
contribution to MAME proper would need to be written again to that project's
standards.

## License

VecBeamMAME is a fork of MAME and is distributed under the same terms.  The files
added or modified by this fork carry their own SPDX headers; the new ones are
made available under the
[3-clause BSD License](http://opensource.org/licenses/BSD-3-Clause), the same
license the great majority of MAME uses.  VecBeamMAME is not an official MAME
release and is not endorsed by the MAME project; please do not report issues
with it to MAMEdev.

The MAME project as a whole is made available under the terms of the
[GNU General Public License, version 2](http://opensource.org/licenses/GPL-2.0)
or later (GPL-2.0+), since it contains code made available under multiple
GPL-compatible licenses.  A great majority of the source files (over 90%
including core files) are made available under the terms of the
[3-clause BSD License](http://opensource.org/licenses/BSD-3-Clause), and we
would encourage new contributors to make their contributions available under the
terms of this license.

Please note that MAME is a registered trademark of Gregory Ember, and permission
is required to use the "MAME" name, logo, or wordmark.

<a href="http://opensource.org/licenses/GPL-2.0" target="_blank">
<img align="right" width="100" src="https://opensource.org/wp-content/uploads/2009/06/OSIApproved.svg">
</a>

    Copyright (c) 1997-2026  MAMEdev and contributors

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License version 2, as provided in
    docs/legal/GPL-2.0.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
    more details.

Please see [COPYING](COPYING) for more details.
