# Piano Visualizer Classic

A lightweight piano MIDI visualizer for Windows.

Piano Visualizer Classic displays MIDI notes as falling notes over an 88-key piano keyboard. It provides configurable visual effects, realtime playback with an internal synthesizer, and MP4 video rendering through FFmpeg.

## Screenshots

### Main Playback Window

![Piano Visualizer Classic - Main](screenshots/visualizer.png)

## Features

* Falling MIDI note visualization
* Full 88-key piano keyboard display
* Velocity-based note and keyboard illumination
* Track-based note colors
* C4 position label
* Optional octave guide lines at B-to-C boundaries
* Background image support with adjustable opacity
* Configurable note fall speed
* Visual effects:

  * Smoke Glow
  * Light Cross
  * Spark
  * Ripple
  * Diamond Ripple
  * Note Glow
* Realtime MIDI playback with an internal XAudio2 synthesizer
* Playback audio can be enabled or disabled independently of the visualizer
* MP4 video rendering through FFmpeg
* Optional audio track in rendered videos
* Render progress and cancellation support
* Resolution-independent video rendering

## Download

The latest release is available on the [Releases](../../releases) page.

### Windows 64-bit

Download `PianoVisualizerClassic-v1.0.0.zip`, extract it, and run:

```text
PianoVisualizerClassic.exe
```

No installer is required.

## Requirements

### Running the application

* Windows 10 or later (64-bit)
* FFmpeg (`ffmpeg.exe`) for MP4 video rendering only

The application can be used for MIDI visualization and realtime playback without FFmpeg.

### Building from source

* Visual Studio with C++ desktop development tools
* Windows SDK
* Visual Studio C++ toolset (`v145`)

CMake is not required.

## Running

Run `PianoVisualizerClassic.exe` directly.

Open a Standard MIDI File (`.mid` or `.midi`) from the control window, then adjust the playback position, note fall speed, background, effects, colors, and audio settings as needed.

Several settings can be changed while playback is running.

## FFmpeg

FFmpeg is required only for MP4 video rendering.

`ffmpeg.exe` is **not included** in this repository.

### Installing FFmpeg

1. Download an FFmpeg package that provides `ffmpeg.exe`.
2. Either:

   * place `ffmpeg.exe` in the same directory as `PianoVisualizerClassic.exe`, or
   * add the directory containing `ffmpeg.exe` to the system `PATH`.
3. Start Piano Visualizer Classic.

The application searches for FFmpeg in the following locations:

1. Next to `PianoVisualizerClassic.exe`
2. A directory listed in the system `PATH`

When FFmpeg is unavailable, realtime MIDI visualization and playback remain available. Only video rendering is disabled.

Video frames are sent directly to FFmpeg as BGRA data, so no temporary image sequence is required. When audio is included, Piano Visualizer Classic creates a temporary WAV file and FFmpeg combines it with the rendered video.

## MIDI Playback

Piano Visualizer Classic loads Standard MIDI Files and converts MIDI timing to seconds using the MIDI file's tempo information.

Only notes within the visible 88-key piano range (MIDI notes 21–108) are displayed.

The internal synthesizer is intentionally lightweight. It is designed to provide convenient playback for the visualizer rather than to reproduce a high-end acoustic piano in detail.

## Video Rendering

Video rendering runs on a worker thread so that the main application window remains responsive.

The video renderer uses a separate Direct2D/WIC rendering context from the realtime playback window. Rendered frames are generated from fixed video timestamps, independent of the speed of the realtime UI loop.

The render dialog provides options including:

* Video width
* Video height
* Output file
* Include audio

The current visualizer settings are used when rendering the video, including:

* Effects
* Track colors
* Background image
* Octave guide lines
* Fall speed

## Building

Open the following solution in Visual Studio:

```text
PianoVisualizer.sln
```

Use:

```text
Configuration: Release
Platform: x64
```

Then build the project.

The Release executable is generated at:

```text
bin\x64\Release\PianoVisualizerClassic.exe
```

## Controls

The control dialog provides settings for playback and visualization, including:

* Fall Speed
* Background image
* Background opacity
* Visual effects
* Track colors
* Playback audio and volume
* Octave guide lines
* Video rendering

## License

Piano Visualizer Classic is released under the MIT License.

See [`LICENSE`](LICENSE) for details.
