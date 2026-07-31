BGFX Effects for (nearly) Everyone
==================================

.. contents:: :local:


Introduction
------------

By default, MAME outputs an idealized version of the video as it would be on the
way to the arcade cabinet’s monitor, with minimal modification of the output
(primarily to stretch the game image back to the aspect ratio the monitor would
traditionally have, usually 4:3).  This works well, but misses some of the
nostalgia factor.  Arcade monitors were never ideal, even in perfect condition,
and the nature of a CRT display distorts that image in ways that change the
appearance significantly.

Modern LCD monitors simply do not look the same, and even computer CRT monitors
cannot match the look of an arcade monitor without help.

That’s where the new BGFX renderer with HLSL comes into the picture.

HLSL simulates most of the effects that a CRT arcade monitor has on the video,
making the result look a lot more authentic.  However, HLSL requires some effort
on the user’s part: the settings you use are going to be tailored to your PC’s
system specs, and especially the monitor you’re using.  Additionally, there were
hundreds of thousands of monitors out there in arcades.  Each was tuned and
maintained differently, meaning there is no one correct appearance to judge by
either.  Basic guidelines will be provided here to help you, but you may also
wish to ask for opinions on popular MAME-centric forums.


Resolution and Aspect Ratio
---------------------------

Resolution is a very important subject for HLSL settings.  You will want MAME to
be using the native resolution of your monitor to avoid additional distortion
and lag caused by your monitor upscaling the display image.

While most arcade machines used a 4:3 ratio display (or 3:4 for vertically
oriented monitors like Pac-Man), it’s difficult to find a consumer display that
is 4:3 at this point.  The good news is that that extra space on the sides isn’t
wasted.  Many arcade cabinets used bezel artwork around the main display, and
should you have the necessary artwork files, MAME will display that artwork.
Turn the **Zoom to Screen Area** setting in the video options menu to scale and
crop the artwork so the emulated screen fills your display in one direction.

Some older LCD displays used a native resolution of 1280×1024 and were a 5:4
aspect ratio.  There’s not enough extra space to display artwork, and you’ll end
up with some very slight pillarboxing, but the results will be still be good and
on-par with a 4:3 monitor.


Getting Started with BGFX
-------------------------

You will need to have followed the initial MAME setup instructions elsewhere in
this manual before beginning.  Official MAME distributions include BGFX as of
MAME 0.172, so you don’t need to download any additional files.

Open your ``mame.ini`` file in your text editor of choice (e.g. Notepad), and
make sure the following options are set correctly:

* ``video bgfx``

Now, you may want to take a moment to look below at the Configuration Settings
section to see how to set up these next options.

As explained in :ref:`advanced-multicfg-order`, MAME has a order in which it
processes INI files.  The BGFX settings can be edited in ``mame.ini``, but to
take full advantage of the power of MAME’s configuration files, you’ll want to
copy the BGFX settings from ``mame.ini`` to one of the other configuration files
and make changes there.

In particular, you will want the ``bgfx_screen_chains`` to be specific to each
game.

Save your INI file(s) and you’re ready to begin.


Configuration Settings
----------------------

bgfx_path
    This is where your BGFX shader files are stored.  By default, this will be
    the *bgfx* folder in your MAME installation folder.
bgfx_backend
    Selects a rendering backend for BGFX to use.  Possible choices include
    ``auto``, ``d3d9``, ``d3d11``, ``d3d12``, ``opengl``, ``gles``, ``metal``,
    and ``vulkan``. The default is ``auto``, which will let MAME choose the
    best selection for you.

    * ``d3d9`` -- Direct3D 9.0 Renderer (May require the DirectX End-User
      Runtime to be installed)
    * ``d3d11`` -- Direct3D 11.0 Renderer
    * ``d3d12`` -- Direct3D 12.0 Renderer
    * ``opengl`` -- OpenGL Renderer (Requires OpenGL drivers, may work better on
      some video cards, supported on Linux and macOS)
    * ``gles`` -- OpenGL ES Renderer (Supported with some low-power GPUs)
    * ``metal`` -- Apple Metal Graphics API (Requires macOS 10.11 El Capitan or
      newer)
    * ``vulkan`` -- Vulkan Renderer (Requires Windows or Linux with compatible
      GPU drivers)
    * ``auto`` -- MAME will automatically choose the best selection for you.
bgfx_debug
    Enables BGFX debugging features.  Most users will not need to use this.
bgfx_screen_chains
    This dictates how to handle BGFX rendering on a per-display basis.  Possible
    choices include ``hlsl``, ``unfiltered``, and ``default``.

    * ``default`` -- **default** bilinear filtered output
    * ``unfiltered`` -- nearest neighbor sampled output
    * ``hlsl`` -- display simulation through shaders
    * ``crt-geom`` -- lightweight CRT simulation
    * ``crt-geom-deluxe`` -- more detailed CRT simulation
    * ``lcd-grid`` -- LCD matrix simulation

    We make a distinction between emulated screens (which we’ll call a *screen*)
    and output windows or monitors (which we’ll call a *window*, set by the
    ``-numscreens`` option) here.  Use colons (:) to separate windows, and
    commas (,) to separate screens in the ``-bgfx_screen_chains`` setting value.

    For the simple single window, single screen case, such as Pac-Man on one
    physical PC monitor, you can specify one entry like::

        bgfx_screen_chains hlsl

    Things get only slightly more complicated when we get to multiple windows
    and multiple screens.

    On a single window, multiple screen game, such as Darius on one physical PC
    monitor, specify screen chains (one per window) like::

        bgfx_screen_chains hlsl,hlsl,hlsl

    This also works with single screen games where you are mirroring the output
    to more than one physical display.  For instance, you could set up Pac-Man
    to have one unfiltered output for use with video broadcasting while a second
    display is set up HLSL for playing on.

    On a multiple window, multiple screen game, such as Darius on three physical
    PC monitors, specify multiple entries (one per window) like::

        bgfx_screen_chains hlsl:hlsl:hlsl

    Another example game would be Taisen Hot Gimmick, which used two CRTs to
    show individual player hands to just that player.  If using two windows (two
    physical displays)::

        bgfx_screen_chains hlsl:hlsl

    One more special case is that Nichibutsu had a special cocktail mahjong
    cabinet that used a CRT in the middle along with two LCD displays to show
    each player their hand.  We would want the LCDs to be unfiltered and
    untouched as they were, while the CRT would be improved through HLSL.  Since
    we want to give each player their own full screen display (two physical
    monitors) along with the LCD, we’ll go with::

        -numscreens 2 -view0 "Player 1" -view1 "Player 2" -video bgfx -bgfx_screen_chains hlsl,unfiltered:hlsl,unfiltered

    This sets up the view for each display respectively, keeping HLSL effect on
    the CRT for each window (physical display) while going unfiltered for the
    LCD screens.

    If using only one window (one display), keep in mind the game still has
    three screens, so we would use::

        bgfx_screen_chains hlsl,unfiltered,unfiltered

    Note that the commas are on the outside edges, and any colons are in the
    middle.
bgfx_render_scale
    Sets the internal resolution scale for the analytic vector renderer and its
    native BGFX chain targets.  The default is ``1.0``.  For example, ``0.5``
    processes the vector chain at half the window width and height while keeping
    the final output, user interface and artwork at full window resolution.
    Target scales declared by a chain are applied on top of this value.

    The vector FBO supersampling factor is applied after this scale.  For the
    lowest fill-rate cost, use ``-bgfx_render_scale 0.5``
    ``-bgfx_vec_supersample 1`` together.

bgfx_shadow_mask
    This specifies the shadow mask effect PNG file.  By default this is
    **slot-mask.png**.


VecBeamMAME analytic vector chains
----------------------------------

VecBeamMAME provides four recommended ``balanced`` BGFX chains for vector
screens:

* ``vector-color-balanced``
* ``vector-monochrome-balanced_1``
* ``vector-monochrome-balanced_2``
* ``vector-vectrex-balanced``

Select the chain in the video options menu, or specify it with
``-video bgfx -bgfx_screen_chains chain-name``.  The chain defaults include the
current *Star Wars*, *Asteroids* and Vectrex calibration work.  Per-system CFG
slider values continue to override the chain defaults.

Overload-driven HV droop
~~~~~~~~~~~~~~~~~~~~~~~~

HV droop globally dims and defocuses the vectors to represent EHT supply sag.
Ordinary lines do not contribute to the effect.  The analytic renderer
integrates only the energy above ``Overload Threshold`` for non-point line
primitives:

.. code-block:: text

    overload load =
        sum(max(line energy - overload threshold, 0)
            * line length / reference screen width)

    droop load =
        clamp((peak-tracked overload load - HV Droop Overload Onset)
              / HV Droop Load Ref, 0, 1)

``HV Droop (dim+defocus)``
    Sets the maximum effect strength.  At 1.0 and full load, line deposits are
    dimmed by up to 40% and the spot sigma gains approximately 2.5 pixels at
    the 1920-pixel reference width.

``HV Droop Overload Onset``
    Rejects isolated overload.  A total overload at or below the onset has
    exactly no global dimming or defocus.

``HV Droop Load Ref``
    Sets the additional overload above the onset required to reach full droop.

The load source is active only when ``Overdrive (hot core)`` is enabled.
Stationary dwell points are excluded, preventing a single hot dot or bullet
from dimming the whole face.  The peak-tracked value decays after a mass
overload event to model supply recovery.

Glow and point optics
~~~~~~~~~~~~~~~~~~~~~

``Glow Narrow`` and ``Glow Wide`` provide the near and broad line glow.  The
extended wide-glow reach replaces the separate local Convergence Bloom effect,
so the local bloom and its tuning controls are not exposed by the vector
chains.

The balanced chains retain ``Convergence Global Bloom`` and
``Convergence Global Coverage``.  This path responds only to a large connected
overload region and produces a broad full-face scatter, without adding a local
convergence halo to compact objects.

For chains with point optics, the slider menu groups the controls in this
order:

#. Halation gain and ring controls
#. Starburst gain, count, length, randomness, width and angle
#. Ordinary analytic glow controls

Deflection Dynamics and its settle/damping controls are not exposed by the
vector chains.  Vector geometry therefore follows the source trajectory
without the optional renderer-side second-order deflection simulation.


Tweaking BGFX HLSL Settings inside MAME
---------------------------------------

Start by loading MAME with the game of your choice (e.g. **mame pacman**).

The tilde key (**~**) brings up the on-screen display options.  Use up and down
to go through the various settings, while left and right will allow you to
change that setting.  Results will be shown in real time as you’re changing
these settings.

Note that settings are individually changeable on a per-screen basis.

BGFX slider settings are saved per-system in CFG files.  If the
``bgfx_screen_chains`` setting has been set (either in an INI file or on the
command line), it will set the initial effects.  If the ``bgfx_screen_chains``
setting has not been set, MAME will use the effects you chose the last time you
ran the system.


Using the included pillarbox filters
------------------------------------

MAME includes example BGFX shaders and layouts for filling unused space on a
16:9 widescreen display with a blurred version of the emulated video.  The all
the necessary files are included, and just need to be enabled.

For systems using 4:3 horizontal monitors, use these options::

    -override_artwork bgfx/border_blur -view Horizontal -bgfx_screen_chains crt-geom,pillarbox_left_horizontal,pillarbox_right_horizontal

For systems using 3:4 vertical monitors, use these options::

    -override_artwork bgfx/border_blur -view Vertical -bgfx_screen_chains crt-geom,pillarbox_left_vertical,pillarbox_right_vertical

* You can use a different setting in place of ``crt-geom`` for the effect to
  apply to the primary screen image in the centre (e.g. ``default``, ``hlsl`` or
  ``lcd-grid``).
* If you’ve previously changed the view for the system in MAME, the correct
  pillarboxed view will not be selected by default.  Use the video options menu
  to select the correct view.
* You can add these settings to an INI file to have them apply to certain
  systems automatically (e.g. **horizont.ini** or **vertical.ini**, or the INI
  file for a specific system).
