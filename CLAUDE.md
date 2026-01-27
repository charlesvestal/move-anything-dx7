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
- `algorithm` (1-32) - FM algorithm
- `feedback` (0-7) - Op6 feedback
- `osc_sync` (0-1) - Oscillator sync
- `transpose` (0-48) - Transpose (24 = middle C)

**LFO**
- `lfo_speed` (0-99) - LFO rate
- `lfo_delay` (0-99) - LFO onset delay
- `lfo_pmd` (0-99) - Pitch mod depth
- `lfo_amd` (0-99) - Amp mod depth
- `lfo_pms` (0-7) - Pitch mod sensitivity
- `lfo_wave` (0-5) - Waveform (tri, saw down, saw up, square, sine, s&h)
- `lfo_sync` (0-1) - LFO key sync

**Pitch Envelope**
- `pitch_eg_r1` through `pitch_eg_r4` (0-99) - Pitch EG rates
- `pitch_eg_l1` through `pitch_eg_l4` (0-99) - Pitch EG levels

**Per-Operator** (op1 through op6)
- `opN_level` (0-99) - Output level
- `opN_coarse` (0-31) - Frequency coarse
- `opN_fine` (0-99) - Frequency fine
- `opN_detune` (-7 to +7) - Fine detune
- `opN_osc_mode` (0-1) - 0=ratio, 1=fixed frequency
- `opN_eg_r1` through `opN_eg_r4` (0-99) - EG rates (attack, decay1, decay2, release)
- `opN_eg_l1` through `opN_eg_l4` (0-99) - EG levels
- `opN_vel_sens` (0-7) - Velocity sensitivity
- `opN_amp_mod` (0-3) - Amp mod sensitivity
- `opN_rate_scale` (0-7) - Rate scaling
- `opN_key_bp` (0-99) - Keyboard breakpoint
- `opN_key_ld` (0-99) - Left depth
- `opN_key_rd` (0-99) - Right depth
- `opN_key_lc` (0-3) - Left curve
- `opN_key_rc` (0-3) - Right curve

## Bank Management

- Scans `<module_dir>/banks/` for .syx files on startup
- Falls back to `patches.syx` in module dir if no banks found
- Bank selection via `syx_bank_index`, `next_syx_bank`, `prev_syx_bank`
- Bank list exposed via `syx_bank_list` for Shadow UI menu

## Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "sound_generator"` in module.json.

## License

GPL-3.0 (inherited from Dexed)
