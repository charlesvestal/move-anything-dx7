# CLAUDE.md

## Project Overview

DX7 FM synthesizer module for Move Anything. Uses MSFA (Music Synthesizer for Android) engine.

## Build Commands

```bash
./scripts/build.sh      # Build with Docker
./scripts/install.sh    # Deploy to Move
```

## Structure

```
src/
  module.json           # Module metadata
  ui.js                 # JavaScript UI
  dsp/
    dx7_plugin.cpp      # Plugin wrapper
    msfa/               # MSFA FM synth engine
```

## DSP Plugin API

Standard Move Anything plugin_api_v1:
- `on_load()`: Initialize synth, load default .syx
- `on_midi()`: Process MIDI input
- `set_param()`: Set preset, syx_path
- `get_param()`: Get preset, preset_count
- `render_block()`: Render 128 frames stereo
