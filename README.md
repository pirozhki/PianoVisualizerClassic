# Piano Visualizer Classic v1.3.0

A lightweight piano MIDI visualizer for Windows. MIDI notes fall toward an 88-key keyboard, with configurable visual effects, real-time playback through a lightweight internal synthesizer, and MP4 video rendering through FFmpeg.

## Screenshots

![Piano Visualizer Classic - Main](https://raw.githubusercontent.com/pirozhki/PianoVisualizerClassic/main/screenshots/visualizer.png)

## Features

* Falling MIDI note visualization for the visible piano range (MIDI notes 21–108)
* Full 88-key keyboard with velocity-based note illumination; active white and black keys use solid track-color fills for consistent visibility
* Track-based note colors and a C4 position label
* Optional octave guide lines at B-to-C boundaries
* Background images and videos can be loaded together: the image is the base layer and the video is composited above it, each with an independent opacity slider; **Default BG** clears both layers
* Replacing an image or video preserves the respective opacity value and video sync offset; loading a different WAV preserves the WAV sync offset. Wave Line amplitude can be adjusted from 0–200% (default 108%) with a dedicated slider and is saved in the INI. The new scale maps 100% to the amplitude previously produced by 200%, so 200% now allows twice that previous maximum.
* Background-video sync offset adjustable from −10,000 to +10,000 ms in 1 ms steps; the current value is displayed beside the slider
* Configurable note fall speed
* Automatic INI settings persistence for visual effects, colors, audio volume, fall speed, Note Glow intensity, Wave Line Glow enable/disable, Keybed Glow, separate image/video paths and opacity, video offset, and the positions of both the visualizer and control windows; existing backgrounds and the last MIDI file are restored on startup when the files still exist
* Configurable visual effects:
  * **Wave Line** — aqua/cyan ripple with a 0–200% amplitude slider (default 108%; 100% equals the previous 200% output), an independent glow checkbox, and fading afterimages
  * **Octave Guide Lines** — vertical guides at octave boundaries
  * **Smoke Glow** and **Light Cross**
  * **Smoke Window** — a randomized smoke texture generated at initialization and held fixed during playback and rendering
  * **White Particles** — slowly moving particles behind notes in the note area only, with a softly faded outer edge; count adjustable from 0 to 200 (default 40)
  * **Impact Polygons** — three fixed-size triangular shards per hit, with a short lifetime
  * **Keybed Glow** — a soft, pure-white radial glow centered at the note/keyboard contact point; it spreads evenly upward, downward, left, and right while the note is held, then disappears when the note turns off
  * **Water Droplets**, **Ripple**, **Diamond Ripple**, and **Note Glow**
* Default enabled effects: **Wave Line**, **Octave Guide Lines**, **Smoke Glow**, **Light Cross**, and **Keybed Glow**
* Real-time MIDI playback using the internal XAudio2 synthesizer, including MIDI sustain/damper pedal (CC64) for live playback and rendered audio; audio can be enabled or disabled independently
* Improved the lightweight internal piano synth with a softer hammer-like attack, pitch-dependent string inharmonicity, a two-stage decay, and velocity-dependent brightness. Upper partials and unison-string beating are strongly attenuated in the treble to keep the fundamental clear. The same simplified model is used for live playback and rendered audio.
* Load an external WAV soundtrack (uncompressed PCM 8/16/24/32-bit or IEEE-float 32/64-bit WAV); while loaded, the enabled internal-synth checkbox selects between WAV playback (unchecked) and the internal synth (checked)
* Shared audio-volume control for MIDI synth and external WAV playback, plus WAV sync offset adjustable from −10,000 to +10,000 ms
* WAV selection and offset are saved in the INI settings; loading a WAV automatically selects it, and the enabled checkbox switches between WAV and internal synth
* MP4 video rendering through FFmpeg, with optional audio, progress reporting, and cancellation
* A configurable visual-effects tail after the final MIDI event (3000 ms by default)

## Download

For a prebuilt Windows binary, see the [Releases](../../releases) page.

The `PianoVisualizerClassic-v1.3.0-source.zip` archive contains the Visual Studio source project and does not include a prebuilt executable.

## Requirements

### Running

* Windows 10 or later (64-bit)
* FFmpeg (`ffmpeg.exe`) for MP4 video rendering only

MIDI visualization and real-time playback do not require FFmpeg.

### Building

* Visual Studio with C++ desktop development tools
* Windows SDK
* Visual Studio C++ toolset (`v145`)

CMake is not required.

## Running

Run `PianoVisualizerClassic.exe` and start by opening a Standard MIDI File (`.mid` or `.midi`) with **Open MIDI...**. The MIDI controls remain visible at the top of the window. The **Background** page is selected initially, so you can next choose **Image...** and/or **Video...**; both can remain loaded together. The image forms the base layer and the video is composited over it. Adjust **Image opacity** and **Video opacity** separately. **Default BG** clears both layers and restores the built-in background. For video backgrounds, use **Video offset** to shift the video relative to the MIDI timeline in 1 ms steps (−10,000 to +10,000 ms); positive values delay the video, while negative values advance it. Use **Effects / Colors** to adjust visual effects and track colors. The optional **Audio / WAV** page is for loading an external WAV soundtrack instead of the internal synth; use **Load WAV...** to select a file. While a WAV is loaded, the internal-synth checkbox stays enabled as an audio-source selector. The **WAV offset** slider shifts the audio relative to the MIDI timeline (positive values delay the WAV; negative values advance it), and **Unload** removes the WAV and restores the internal-synth choice. Adjust playback position, fall speed, background opacity, effects, colors, and audio settings as needed. Several settings can be changed while playback is running.

Preferences are saved automatically to `PianoVisualizerClassic.ini` beside the executable. Changes are written shortly after you stop adjusting a setting and are saved again when the application closes. The main visualizer window and the separate control window save their last normal top-left positions and restore them at startup. If a saved location is outside the currently connected monitors, the window is moved back into a visible work area. The last MIDI path is saved and automatically loaded at startup if the file still exists. Image and video backgrounds, their separate opacity values, video offset, WAV path, and WAV offset are persisted and restored independently where possible. Custom track colors are remembered separately for each MIDI file path.

## External WAV Audio

The WAV loader supports uncompressed PCM and IEEE-float WAV files and converts them to 48 kHz stereo for live playback. Compressed WAV codecs such as ADPCM are not supported. The shared volume slider affects either the WAV or the internal synth. The WAV offset range is −10,000 to +10,000 ms. MP4 rendering uses the selected WAV when one is loaded and applies the same offset and volume.

## FFmpeg

FFmpeg is required only for MP4 rendering and is **not included** in this repository. Place `ffmpeg.exe` next to `PianoVisualizerClassic.exe` or add its directory to the system `PATH`. The application searches those locations. When FFmpeg is unavailable, real-time visualization and playback remain available; only video rendering is disabled.

Video frames are sent directly to FFmpeg as BGRA data, without a temporary image sequence. When audio is included, FFmpeg combines the audio with the rendered video. The soundtrack of the background video is not used. When no external WAV is loaded, the optional audio track is generated by the internal synthesizer; otherwise the selected WAV is included with its configured offset and shared volume setting. Live playback and MP4 rendering preserve the background-video timeline and any selected WAV throughout the configured visual tail.

## Video Rendering

Video rendering runs on a worker thread with its own Direct2D/WIC rendering context. Frames are generated from fixed timestamps rather than the real-time UI loop, while using the same frame-update and drawing path as the live preview. Rendering options include output width and height, destination file, and whether to include audio. Current visualizer settings—including effect toggles and strengths, Wave Line amplitude, Keybed Glow, track colors, background image and video layers with separate opacities, video sync offset, octave guides, particle count, and fall speed—are copied to the render context.

Video backgrounds are synchronized to the MIDI timeline using the configured offset. If a background video ends before the MIDI and visual tail, its final frame remains displayed for the rest of playback or rendering; it does not loop. Windows Media Foundation decodes the background video, and available formats depend on installed Windows codecs (MP4/H.264 is recommended). The default MP4 output size is 1280×720 at 60 fps.

## Controls

The compact control window keeps MIDI loading, playback, position, and fall speed visible at the top. Settings are divided into **Background**, **Effects / Colors**, and **Audio / WAV** pages so the window fits more comfortably on smaller displays. The **Background** page opens by default to match the typical workflow: load a MIDI file first, then choose a background. The outro duration can be entered in milliseconds next to **Render Video...** (0–60000 ms; default 3000 ms). **Audio / WAV** is an optional page for loading or unloading an external soundtrack and adjusting its volume and offset.

## Release Notes — v1.3.0

* **More reliable MIDI and settings recovery:** Fixed tempo-map handling; the last MIDI file, settings, per-song track colors, media paths, and window positions are saved and restored when available.
* **Layered backgrounds and external audio:** Image and video backgrounds can be shown together with independent opacity controls. Added WAV playback, sync offset, shared volume, live source switching, and MIDI sustain pedal (CC64) support for the internal synth.
* **Improved lightweight piano synth:** Softer attack, pitch-dependent string inharmonicity, velocity-sensitive tone, and a two-stage decay. Upper partials and treble string beating are reduced to make the sound less harsh while keeping real-time playback lightweight.
* **Visual and control improvements:** Adjustable Note Glow and Wave Line amplitude, independent Wave Line Glow, brighter symmetric white Keybed Glow, more visible pressed black keys, and the smaller **Water Droplets** effect. Settings pages are organized into tabs, and replacing media preserves opacity and offset values.
* **Configurable outro tail and rendering:** Tail duration can be set from 0 to 60,000 ms (3,000 ms by default). Background video and WAV continue through the tail, playback remains `Playing` until the tail ends, and MP4 export uses the configured duration and selected audio source.

For earlier release notes, see the [GitHub Releases](../../releases) page.

## License

Piano Visualizer Classic is released under the MIT License. See [`LICENSE`](LICENSE) for details.

### Control dialog pages

The control dialog uses the native Windows `SysTabControl32` for Background,
Effects / Colors, and Audio / WAV. The settings are visually placed within the tab
page frame. Switching pages resizes both the page frame and dialog as needed
without recentering, preserving the window's current top-left position.


### Internal synth follow-up

* Reduced upper-partial levels substantially and attenuated them further with increasing pitch; also reduced high-register string detuning to minimize conspicuous ringing/chorus while keeping the fundamental level unchanged.
