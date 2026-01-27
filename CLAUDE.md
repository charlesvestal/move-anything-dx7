# CLAUDE.md

## Project Overview

Dexed FM synthesizer module for Move Anything. Uses the MSFA (Music Synthesizer for Android) engine.

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
banks/                  # User .syx files (created on install)
```

## DSP Plugin API

Standard Move Anything plugin_api_v2 (multi-instance):
- `create_instance()`: Initialize synth, scan banks, load default .syx
- `destroy_instance()`: Cleanup
- `on_midi()`: Process MIDI input
- `set_param()`: Set preset, bank, DX7 parameters
- `get_param()`: Get preset, preset_count, ui_hierarchy, chain_params, parameter values
- `render_block()`: Render 128 frames stereo

## Parameters

Parameters organized into Shadow UI hierarchy categories:

**Global**
- `output_level` (0-100) - Output volume
- `octave_transpose` (-3 to +3) - Octave shift
- `algorithm` (1-32) - FM algorithm (read-only)
- `feedback` (0-7) - Op6 feedback

**LFO**
- `lfo_speed` (0-99) - LFO rate
- `lfo_delay` (0-99) - LFO onset delay
- `lfo_pmd` (0-99) - Pitch mod depth
- `lfo_amd` (0-99) - Amp mod depth
- `lfo_wave` (0-5) - Waveform

**Operators**
- `op1_level` through `op6_level` (0-99) - Operator output levels

## Bank Management

- Scans `<module_dir>/banks/` for .syx files on startup
- Falls back to `patches.syx` in module dir if no banks found
- Bank selection via `syx_bank_index`, `next_syx_bank`, `prev_syx_bank`
- Bank list exposed via `syx_bank_list` for Shadow UI menu

## Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "sound_generator"` in module.json.

## License

GPL-3.0 (inherited from Dexed)
