# Dexed Module for Move Anything

Dexed FM synthesizer module for Ableton Move, using the MSFA engine from Dexed.

## Features

- Full 6-operator FM synthesis
- All 32 classic 6-operator FM algorithms
- Loads standard .syx patch banks (32 voices per bank)
- 16-voice polyphony with voice stealing
- Velocity sensitivity and aftertouch modulation
- Pitch bend, mod wheel, sustain pedal support
- Octave transpose (-4 to +4)
- Signal Chain compatible

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Installation

### Via Module Store (Recommended)

1. Launch Move Anything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Sound Generators** → **Dexed**
4. Select **Install**

### Manual Installation

```bash
./scripts/install.sh
```

## Loading Patches

Dexed loads `patches.syx` from its module directory on startup.

### Quick Setup

1. Download a DX7-compatible .syx bank (see "Finding Patches" below)
2. Copy to Move:
   ```bash
   scp your-patches.syx ableton@move.local:/data/UserData/move-anything/modules/dexed/patches.syx
   ```
3. Restart the Dexed module to load the new bank

### Patch File Format

The module expects standard DX7-compatible 32-voice bank sysex files:
- 4104 bytes total (with sysex headers)
- 4096 bytes of patch data (32 patches x 128 bytes)
- Standard VMEM packed format

Single-voice VCED dumps are not supported.

## Controls

### Move Hardware

| Control | Action |
|---------|--------|
| **Jog wheel** | Navigate presets (1-32) |
| **Left/Right** | Previous/next preset |
| **Up/Down** | Octave transpose |
| **Pads** | Play notes (velocity sensitive, with aftertouch) |

### External MIDI

| Control | Function |
|---------|----------|
| Note On/Off | Play notes |
| Velocity | Controls operator output levels |
| Pitch Bend | +/- 2 semitones |
| Mod Wheel (CC 1) | LFO pitch/amplitude modulation |
| Aftertouch | Pitch/amplitude modulation |
| Sustain (CC 64) | Hold notes |

## Finding Patches

Thousands of free DX7 .syx patches are available online:

- https://homepages.abdn.ac.uk/d.j.benson/pages/html/dx7.html
- https://yamahablackboxes.com/collection/yamaha-dx7-dx9-dx7ii-patches/
- https://www.polynominal.com/yamaha-dx7/

Classic banks include:
- ROM1A/ROM1B - Original factory presets
- E! series - Famous aftermarket patches

## Troubleshooting

**No sound:**
- Ensure a .syx file is loaded at `modules/dexed/patches.syx`
- The default "Init" patch is very quiet - try changing presets

**Clipping with chords:**
- Dexed can be loud with multiple voices
- Use in Signal Chain with lower mix settings

**Wrong pitch:**
- Ensure you're using standard DX7-compatible .syx files (not TX81Z or other FM synths)

## Building from Source

```bash
./scripts/build.sh
```

Requires Docker or ARM64 cross-compiler.

## Credits

- MSFA engine: Google (Apache 2.0)
- Dexed: Pascal Gauthier / asb2m10
