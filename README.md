# Piano Visualizer Classic v1.1.0

A lightweight piano MIDI visualizer for Windows. MIDI notes fall toward an 88-key keyboard, with configurable visual effects, realtime playback through a lightweight internal synthesizer, and MP4 video rendering through FFmpeg.

## Screenshots

![Piano Visualizer Classic - Main](screenshots/visualizer.png)

## Features

* Falling MIDI note visualization for the visible piano range (MIDI notes 21–108)
* Full 88-key keyboard with velocity-based note and keyboard illumination
* Track-based note colors and a C4 position label
* Optional octave guide lines at B-to-C boundaries
* Background images with adjustable opacity; **Default BG** restores the built-in dark background
* Configurable note fall speed
* Configurable visual effects:
  * **Wave Line** — aqua/cyan ripple with subtle fading afterimages; brighter when Note Glow is enabled
  * **Octave Guide Lines** — vertical guides at octave boundaries
  * **Smoke Glow** and **Light Cross**
  * **Smoke Window** — one large, fixed smoke image revealed through translucent falling notes
  * **White Particles** — independently randomized, slowly moving particles behind notes in the note area only; count adjustable from 0 to 200 (default 40)
  * **Impact Polygons** — three fixed-size triangular shards per hit, with a short lifetime
  * **Spark**, **Ripple**, **Diamond Ripple**, and **Note Glow**
* Default enabled effects: **Wave Line**, **Octave Guide Lines**, **Smoke Glow**, and **Light Cross**
* Realtime MIDI playback using the internal XAudio2 synthesizer; audio can be enabled or disabled independently
* MP4 video rendering through FFmpeg, with optional audio, progress reporting, and cancellation
* A 2-second visual-effects tail after the final MIDI event

## Download

For a prebuilt Windows binary, see the [Releases](../../releases) page.

The `PianoVisualizerClassic-v1.1.0-source.zip` archive contains the Visual Studio source project and does not include a prebuilt executable.

## Requirements

### Running

* Windows 10 or later (64-bit)
* FFmpeg (`ffmpeg.exe`) for MP4 video rendering only

MIDI visualization and realtime playback do not require FFmpeg.

### Building

* Visual Studio with C++ desktop development tools
* Windows SDK
* Visual Studio C++ toolset (`v145`)

CMake is not required.

## Running

Run `PianoVisualizerClassic.exe`, then open a Standard MIDI File (`.mid` or `.midi`) from the control window. Adjust playback position, fall speed, background, effects, colors, and audio settings as needed. Several settings can be changed while playback is running.

## FFmpeg

FFmpeg is required only for MP4 rendering and is **not included** in this repository. Place `ffmpeg.exe` next to `PianoVisualizerClassic.exe` or add its directory to the system `PATH`. The application searches those locations. When FFmpeg is unavailable, realtime visualization and playback remain available; only video rendering is disabled.

Video frames are sent directly to FFmpeg as BGRA data, without a temporary image sequence. When audio is included, the application creates a temporary WAV file and FFmpeg combines it with the rendered video.

## Video Rendering

Video rendering runs on a worker thread with its own Direct2D/WIC rendering context. Frames are generated from fixed timestamps rather than the realtime UI loop. Rendering options include output width and height, destination file, and whether to include audio. Current visualizer settings—including effects, track colors, background image and opacity, octave guides, particle count, and fall speed—are copied to the render context.

## Building

Open `PianoVisualizer.sln` in Visual Studio and build with:

```text
Configuration: Release
Platform: x64
```

The executable is generated at:

```text
bin\x64\Release\PianoVisualizerClassic.exe
```

## Controls

The control window provides MIDI loading and playback controls, fall speed, background image and opacity controls, the **Default BG** reset button, effect toggles, White Particles count, Wave Line color selection, track colors, audio volume, octave guide lines, and video rendering.

## Release Notes — v1.1.0

This release consolidates the improvements made during development:

* Added the configurable aqua Wave Line with reduced amplitude, slower propagation, stronger distance attenuation, subtle afterimages, and stronger glow when Note Glow is enabled. Ripple and Diamond Ripple show expanding outlines without a center flash.
* Added Smoke Window using one bright, fixed smoke texture; the visible portion changes as translucent notes fall across it, and the loaded background image does not show through the smoke backing.
* Added deterministic randomized White Particles with per-particle position, speed, size, drift, and twinkle. Particles are clipped to the note area and rendered behind notes. Their count slider ranges from 0 to 200, defaulting to 40, and applies to offline rendering too.
* Added Impact Polygons: three shards per hit, all at size 6.0, with no size shrink during their short approximately 0.36–0.50-second lifetime.
* Set the default enabled effects to Wave Line, Octave Guide Lines, Smoke Glow, and Light Cross. Light Cross particles rise a little higher.
* Refined Smoke Glow by removing its outline, tightening the blur, and slowing the rise.
* Brightened the white-key gradient without changing black keys, set background-image opacity to 30% on load, and added a button to restore the built-in background.
* Extended the visual-effects tail after the final MIDI event to 2 seconds.
* Added Windows executable version metadata.

## License

Piano Visualizer Classic is released under the MIT License. See [`LICENSE`](LICENSE) for details.
