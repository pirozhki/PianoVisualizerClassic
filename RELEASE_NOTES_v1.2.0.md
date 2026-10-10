# Piano Visualizer Classic v1.2.0 — Release Notes

## Highlights

* **Synchronized video backgrounds:** Load a background video separately from a still image. The video follows MIDI playback and is synchronized in MP4 rendering as well.
* **Video offset:** Adjust video timing from −10,000 to +10,000 ms in 1 ms steps. The signed millisecond value updates beside the slider. Positive values delay the video; negative values advance it.
* **More reliable video navigation:** Seeking and slider scrubbing are handled to keep the UI responsive. At the end of a background video, the final frame remains on screen instead of looping.
* **Background controls:** Separate **Image...** and **Video...** buttons, plus **Default BG** to restore the built-in background.
* **White Particles:** A softer outer edge, while keeping the established central glow and cross sparkle. Particles remain behind the falling notes and limited to the note area.
* **Smoke Window:** The smoke texture is randomized at initialization, then remains fixed for stable playback and rendering.
* **Rendering:** Live preview and MP4 output use the same frame-update and drawing path. The visual-effects tail is 3 seconds, and the default output size is 1280×720 at 60 fps.

## Requirements

* Windows 10 or later (64-bit)
* FFmpeg (`ffmpeg.exe`) for MP4 rendering only

The source archive contains the Visual Studio project, not a prebuilt executable. See `README.md` for building and usage details.
