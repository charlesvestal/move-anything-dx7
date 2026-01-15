/*
 * DX7 Synth DSP Plugin
 *
 * Uses msfa (Music Synthesizer for Android) FM engine from Dexed.
 * Provides 6-operator FM synthesis with DX7 patch compatibility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <memory>

/* Include plugin API */
extern "C" {
/* Copy plugin_api_v1.h definitions inline to avoid path issues */
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    void (*render_block)(int16_t *out_interleaved_lr, int frames);
} plugin_api_v1_t;

/* Plugin API v2 - Instance-based for multi-instance support */
#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;
}

/* msfa FM engine */
#include "msfa/synth.h"
#include "msfa/fm_core.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/env.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/freqlut.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include "msfa/tuning.h"

/* Constants */
#define MAX_VOICES 16
#define DX7_PATCH_SIZE 156   /* Size of unpacked DX7 voice data */
#define DX7_PACKED_SIZE 128  /* Size of packed DX7 voice in .syx */
#define MAX_PATCHES 128

/* Plugin state */
static const host_api_v1_t *g_host = NULL;
static int g_current_preset = 0;
static int g_preset_count = 0;
static int g_octave_transpose = 0;
static char g_patch_path[512] = {0};
static char g_patch_name[128] = "Init";
static int g_active_voices = 0;

/* Tuning state */
static std::shared_ptr<TuningState> g_tuning;

/* Controllers */
static Controllers g_controllers;

/* FM Core */
static FmCore g_fm_core;

/* LFO */
static Lfo g_lfo;

/* Voices */
static Dx7Note* g_voices[MAX_VOICES];
static int g_voice_note[MAX_VOICES];  /* MIDI note for each voice, -1 = free */
static int g_voice_age[MAX_VOICES];   /* Age counter for voice stealing */
static bool g_voice_sustained[MAX_VOICES];  /* True if held by sustain pedal */
static int g_age_counter = 0;

/* Sustain pedal state */
static bool g_sustain_pedal = false;

/* Current patch data (unpacked 156 bytes) */
static uint8_t g_current_patch[DX7_PATCH_SIZE];

/* All loaded patches */
static uint8_t g_patches[MAX_PATCHES][DX7_PATCH_SIZE];
static char g_patch_names[MAX_PATCHES][11];

/* Rendering buffer */
static int32_t g_render_buffer[N];

/* Output level (0-100, default 50 to allow polyphony headroom) */
static int g_output_level = 50;

/* Plugin API */
static plugin_api_v1_t g_plugin_api;

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        g_host->log(msg);
    } else {
        printf("[dx7] %s\n", msg);
    }
}

/* Initialize lookup tables */
static void init_tables() {
    Exp2::init();
    Sin::init();
    Freqlut::init(MOVE_SAMPLE_RATE);
    Lfo::init(MOVE_SAMPLE_RATE);
    PitchEnv::init(MOVE_SAMPLE_RATE);
    Env::init_sr(MOVE_SAMPLE_RATE);
    Porta::init_sr(MOVE_SAMPLE_RATE);
}

/* Initialize the default "Init" patch */
static void init_default_patch() {
    memset(g_current_patch, 0, DX7_PATCH_SIZE);

    /* Set up a basic init patch - just carrier op 1 */
    for (int op = 0; op < 6; op++) {
        int base = op * 21;

        /* EG rates and levels */
        g_current_patch[base + 0] = 99;  /* R1 */
        g_current_patch[base + 1] = 99;  /* R2 */
        g_current_patch[base + 2] = 99;  /* R3 */
        g_current_patch[base + 3] = 99;  /* R4 */
        g_current_patch[base + 4] = 99;  /* L1 */
        g_current_patch[base + 5] = 99;  /* L2 */
        g_current_patch[base + 6] = 99;  /* L3 */
        g_current_patch[base + 7] = 0;   /* L4 */

        /* Keyboard scaling */
        g_current_patch[base + 8] = 0;   /* BP */
        g_current_patch[base + 9] = 0;   /* LD */
        g_current_patch[base + 10] = 0;  /* RD */
        g_current_patch[base + 11] = 0;  /* LC */
        g_current_patch[base + 12] = 0;  /* RC */

        /* Other params */
        g_current_patch[base + 13] = 0;  /* Rate scaling */
        g_current_patch[base + 14] = 0;  /* Amp mod sens */
        g_current_patch[base + 15] = 0;  /* Key vel sens */
        g_current_patch[base + 16] = (op == 0) ? 99 : 0;  /* Output level - only op1 */
        g_current_patch[base + 17] = 0;  /* Osc mode (ratio) */
        g_current_patch[base + 18] = 1;  /* Freq coarse */
        g_current_patch[base + 19] = 0;  /* Freq fine */
        g_current_patch[base + 20] = 7;  /* Detune (center) */
    }

    /* Pitch EG */
    g_current_patch[126] = 50;  /* PR1 */
    g_current_patch[127] = 50;  /* PR2 */
    g_current_patch[128] = 50;  /* PR3 */
    g_current_patch[129] = 50;  /* PR4 */
    g_current_patch[130] = 50;  /* PL1 */
    g_current_patch[131] = 50;  /* PL2 */
    g_current_patch[132] = 50;  /* PL3 */
    g_current_patch[133] = 50;  /* PL4 */

    /* Algorithm */
    g_current_patch[134] = 0;   /* Algorithm 1 */

    /* Feedback */
    g_current_patch[135] = 0;

    /* Oscillator sync */
    g_current_patch[136] = 1;

    /* LFO */
    g_current_patch[137] = 35;  /* LFO speed */
    g_current_patch[138] = 0;   /* LFO delay */
    g_current_patch[139] = 0;   /* LFO PMD */
    g_current_patch[140] = 0;   /* LFO AMD */
    g_current_patch[141] = 1;   /* LFO sync */
    g_current_patch[142] = 0;   /* LFO wave */
    g_current_patch[143] = 3;   /* LFO PMS */

    /* Transpose */
    g_current_patch[144] = 24;  /* Middle C */

    /* Name: "Init" */
    memcpy(&g_current_patch[145], "Init       ", 10);
}

/* Unpack a 128-byte packed DX7 voice to 156-byte format */
static void unpack_patch(const uint8_t *packed, uint8_t *unpacked) {
    /* Operators 1-6 - same order as Dexed (no reversal) */
    for (int op = 0; op < 6; op++) {
        int p = op * 17;  /* packed offset */
        int u = op * 21;  /* unpacked offset - same order as packed */

        /* EG rates */
        unpacked[u + 0] = packed[p + 0] & 0x7f;
        unpacked[u + 1] = packed[p + 1] & 0x7f;
        unpacked[u + 2] = packed[p + 2] & 0x7f;
        unpacked[u + 3] = packed[p + 3] & 0x7f;

        /* EG levels */
        unpacked[u + 4] = packed[p + 4] & 0x7f;
        unpacked[u + 5] = packed[p + 5] & 0x7f;
        unpacked[u + 6] = packed[p + 6] & 0x7f;
        unpacked[u + 7] = packed[p + 7] & 0x7f;

        /* Keyboard scaling */
        unpacked[u + 8] = packed[p + 8] & 0x7f;    /* BP */
        unpacked[u + 9] = packed[p + 9] & 0x7f;    /* LD */
        unpacked[u + 10] = packed[p + 10] & 0x7f;  /* RD */
        unpacked[u + 11] = packed[p + 11] & 0x03;  /* LC */
        unpacked[u + 12] = (packed[p + 11] >> 2) & 0x03;  /* RC */

        /* Other */
        unpacked[u + 13] = (packed[p + 12] >> 0) & 0x07;  /* Rate scaling */
        unpacked[u + 14] = (packed[p + 13] >> 0) & 0x03;  /* Amp mod sens */
        unpacked[u + 15] = (packed[p + 13] >> 2) & 0x07;  /* Key vel sens */
        unpacked[u + 16] = packed[p + 14] & 0x7f;  /* Output level */
        unpacked[u + 17] = (packed[p + 15] >> 0) & 0x01;  /* Osc mode */
        unpacked[u + 18] = (packed[p + 15] >> 1) & 0x1f;  /* Freq coarse */
        unpacked[u + 19] = packed[p + 16] & 0x7f;  /* Freq fine */
        unpacked[u + 20] = (packed[p + 12] >> 3) & 0x0f;  /* Detune */
    }

    /* Global parameters (offset 102 in packed) */
    int p = 102;

    /* Pitch EG */
    unpacked[126] = packed[p + 0] & 0x7f;
    unpacked[127] = packed[p + 1] & 0x7f;
    unpacked[128] = packed[p + 2] & 0x7f;
    unpacked[129] = packed[p + 3] & 0x7f;
    unpacked[130] = packed[p + 4] & 0x7f;
    unpacked[131] = packed[p + 5] & 0x7f;
    unpacked[132] = packed[p + 6] & 0x7f;
    unpacked[133] = packed[p + 7] & 0x7f;

    /* Algorithm & feedback */
    unpacked[134] = packed[p + 8] & 0x1f;
    unpacked[135] = (packed[p + 8] >> 5) & 0x07;

    /* Osc sync */
    unpacked[136] = packed[p + 9] & 0x01;

    /* LFO */
    unpacked[137] = packed[p + 10] & 0x7f;  /* Speed */
    unpacked[138] = packed[p + 11] & 0x7f;  /* Delay */
    unpacked[139] = packed[p + 12] & 0x7f;  /* PMD */
    unpacked[140] = packed[p + 13] & 0x7f;  /* AMD */
    unpacked[141] = (packed[p + 9] >> 1) & 0x01;   /* LFO sync */
    unpacked[142] = packed[p + 14] & 0x07;         /* LFO wave (bits 0-2) */
    unpacked[143] = (packed[p + 14] >> 4) & 0x07;  /* LFO PMS (bits 4-6) */

    /* Transpose */
    unpacked[144] = packed[p + 15] & 0x7f;

    /* Name (10 chars) */
    for (int i = 0; i < 10; i++) {
        unpacked[145 + i] = packed[p + 16 + i];
    }
}

/* Load a .syx file containing 32 DX7 voices */
static int load_syx(const char *path) {
    char msg[256];

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof(msg), "Cannot open: %s", path);
        plugin_log(msg);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Standard DX7 32-voice sysex is 4104 bytes */
    if (size != 4104) {
        snprintf(msg, sizeof(msg), "Invalid syx size: %ld (expected 4104)", size);
        plugin_log(msg);
        fclose(f);
        return -1;
    }

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        fclose(f);
        return -1;
    }

    fread(data, 1, size, f);
    fclose(f);

    /* Verify sysex header: F0 43 00 09 20 00 */
    if (data[0] != 0xF0 || data[1] != 0x43 || data[3] != 0x09) {
        plugin_log("Invalid DX7 sysex header");
        free(data);
        return -1;
    }

    /* Extract 32 patches starting at offset 6 */
    g_preset_count = 32;
    for (int i = 0; i < 32; i++) {
        uint8_t *packed = &data[6 + i * 128];
        unpack_patch(packed, g_patches[i]);

        /* Extract name */
        for (int j = 0; j < 10; j++) {
            char c = g_patches[i][145 + j];
            g_patch_names[i][j] = (c >= 32 && c < 127) ? c : ' ';
        }
        g_patch_names[i][10] = '\0';
    }

    free(data);

    strncpy(g_patch_path, path, sizeof(g_patch_path) - 1);

    snprintf(msg, sizeof(msg), "Loaded 32 patches from: %s", path);
    plugin_log(msg);

    return 0;
}

/* Select a preset by index */
static void select_preset(int index) {
    if (index < 0) index = g_preset_count - 1;
    if (index >= g_preset_count) index = 0;

    g_current_preset = index;
    memcpy(g_current_patch, g_patches[index], DX7_PATCH_SIZE);
    strncpy(g_patch_name, g_patch_names[index], sizeof(g_patch_name) - 1);

    /* Update LFO for new patch */
    g_lfo.reset(g_current_patch + 137);

    char msg[128];
    snprintf(msg, sizeof(msg), "Preset %d: %s (alg %d)",
             index, g_patch_name, g_current_patch[134] + 1);
    plugin_log(msg);
}

/* Find a free voice or steal the oldest */
static int allocate_voice() {
    /* First try to find a free voice */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voice_note[i] < 0) {
            return i;
        }
    }

    /* No free voice - find the oldest playing voice */
    int oldest = 0;
    int oldest_age = g_voice_age[0];
    for (int i = 1; i < MAX_VOICES; i++) {
        if (g_voice_age[i] < oldest_age) {
            oldest = i;
            oldest_age = g_voice_age[i];
        }
    }

    return oldest;
}

/* Note on */
static void note_on(int note, int velocity) {
    /* Count active voices before adding new one */
    int active_before = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voice_note[i] >= 0) active_before++;
    }

    int voice = allocate_voice();
    g_voices[voice]->init(g_current_patch, note, velocity, 0, &g_controllers);
    g_voice_note[voice] = note;
    g_voice_age[voice] = g_age_counter++;
    g_voice_sustained[voice] = false;

    /* Only trigger LFO sync on first voice, not subsequent voices */
    if (active_before == 0) {
        g_lfo.keydown();
    }
}

/* Note off */
static void note_off(int note) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voice_note[i] == note) {
            if (g_sustain_pedal) {
                /* Mark as sustained - will release when pedal is lifted */
                g_voice_sustained[i] = true;
            } else {
                g_voices[i]->keyup();
                g_voice_sustained[i] = false;
            }
            /* Don't clear note yet - let release phase play */
        }
    }
}

/* Release all sustained notes (called when sustain pedal is lifted) */
static void release_sustained_notes() {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voice_sustained[i]) {
            g_voices[i]->keyup();
            g_voice_sustained[i] = false;
        }
    }
}

/* All notes off - just triggers release phase */
static void all_notes_off() {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voice_note[i] >= 0) {
            g_voices[i]->keyup();
        }
        g_voice_sustained[i] = false;
    }
    g_sustain_pedal = false;
}

/* Panic - immediately silence all voices by reinitializing them */
static void panic() {
    for (int i = 0; i < MAX_VOICES; i++) {
        /* Delete and recreate the voice to fully reset it */
        delete g_voices[i];
        g_voices[i] = new Dx7Note(g_tuning, nullptr);
        g_voice_note[i] = -1;
        g_voice_sustained[i] = false;
        g_voice_age[i] = 0;
    }
    g_sustain_pedal = false;
    g_active_voices = 0;
}

/* === Plugin API callbacks === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "DX7 plugin loading from: %s", module_dir);
    plugin_log(msg);

    /* Initialize tables */
    init_tables();

    /* Initialize tuning state */
    g_tuning = std::make_shared<TuningState>();

    /* Initialize controllers */
    g_controllers.core = &g_fm_core;
    g_controllers.masterTune = 0;

    /* Initialize all controller values to sane defaults */
    memset(g_controllers.values_, 0, sizeof(g_controllers.values_));
    g_controllers.values_[kControllerPitch] = 0x2000;  /* Center pitch bend */
    g_controllers.values_[kControllerPitchRangeUp] = 2;  /* +/- 2 semitones default */
    g_controllers.values_[kControllerPitchRangeDn] = 2;
    g_controllers.values_[kControllerPitchStep] = 0;  /* Continuous pitch bend */

    g_controllers.modwheel_cc = 0;
    g_controllers.breath_cc = 0;
    g_controllers.foot_cc = 0;
    g_controllers.aftertouch_cc = 0;
    g_controllers.portamento_cc = 0;
    g_controllers.portamento_enable_cc = false;
    g_controllers.portamento_gliss_cc = false;
    g_controllers.mpeEnabled = false;  /* Disable MPE by default */

    /* Configure modulation sources - DX7-like defaults */
    /* Mod wheel: controls pitch and amp modulation via LFO */
    g_controllers.wheel.range = 99;
    g_controllers.wheel.pitch = true;
    g_controllers.wheel.amp = true;
    g_controllers.wheel.eg = false;

    /* Aftertouch: controls pitch and amp modulation */
    g_controllers.at.range = 99;
    g_controllers.at.pitch = true;
    g_controllers.at.amp = true;
    g_controllers.at.eg = false;

    /* Breath controller: available but typically configured per-patch */
    g_controllers.breath.range = 99;
    g_controllers.breath.pitch = false;
    g_controllers.breath.amp = true;
    g_controllers.breath.eg = false;

    /* Foot controller: available but typically configured per-patch */
    g_controllers.foot.range = 99;
    g_controllers.foot.pitch = false;
    g_controllers.foot.amp = false;
    g_controllers.foot.eg = false;

    g_controllers.refresh();

    /* Initialize voices */
    for (int i = 0; i < MAX_VOICES; i++) {
        g_voices[i] = new Dx7Note(g_tuning, nullptr);
        g_voice_note[i] = -1;
        g_voice_age[i] = 0;
        g_voice_sustained[i] = false;
    }
    g_sustain_pedal = false;

    /* Initialize with default patch */
    init_default_patch();
    memcpy(g_patches[0], g_current_patch, DX7_PATCH_SIZE);
    strcpy(g_patch_names[0], "Init");
    g_preset_count = 1;

    /* Initialize LFO */
    g_lfo.reset(g_current_patch + 137);

    /* Try to load default syx file */
    char default_syx[512] = {0};
    if (json_defaults) {
        const char *pos = strstr(json_defaults, "\"syx_path\"");
        if (pos) {
            pos = strchr(pos, ':');
            if (pos) {
                pos = strchr(pos, '"');
                if (pos) {
                    pos++;
                    int i = 0;
                    while (*pos && *pos != '"' && i < (int)sizeof(default_syx) - 1) {
                        default_syx[i++] = *pos++;
                    }
                    default_syx[i] = '\0';
                }
            }
        }
    }

    if (default_syx[0]) {
        load_syx(default_syx);
    } else {
        char syx_path[512];
        snprintf(syx_path, sizeof(syx_path), "%s/patches.syx", module_dir);
        load_syx(syx_path);
    }

    if (g_preset_count > 0) {
        select_preset(0);
    }

    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("DX7 plugin unloading");

    for (int i = 0; i < MAX_VOICES; i++) {
        delete g_voices[i];
        g_voices[i] = nullptr;
    }
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    if (len < 2) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int is_note = (status == 0x90 || status == 0x80);

    /* Apply octave transpose */
    int note = data1;
    if (is_note) {
        note += g_octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90: /* Note On */
            if (data2 > 0) {
                note_on(note, data2);
            } else {
                note_off(note);
            }
            break;

        case 0x80: /* Note Off */
            note_off(note);
            break;

        case 0xA0: /* Polyphonic Aftertouch - use as channel aftertouch */
            g_controllers.aftertouch_cc = data2;
            g_controllers.refresh();
            break;

        case 0xB0: /* Control Change */
            switch (data1) {
                case 1:  /* Mod wheel */
                    g_controllers.modwheel_cc = data2;
                    g_controllers.refresh();
                    break;
                case 2:  /* Breath controller */
                    g_controllers.breath_cc = data2;
                    g_controllers.refresh();
                    break;
                case 4:  /* Foot controller */
                    g_controllers.foot_cc = data2;
                    g_controllers.refresh();
                    break;
                case 64: /* Sustain pedal */
                    g_sustain_pedal = (data2 >= 64);
                    if (!g_sustain_pedal) {
                        release_sustained_notes();
                    }
                    break;
                case 123: /* All notes off */
                    all_notes_off();
                    break;
            }
            break;

        case 0xD0: /* Channel Aftertouch */
            g_controllers.aftertouch_cc = data1;
            g_controllers.refresh();
            break;

        case 0xE0: /* Pitch Bend */
            {
                int bend = ((int)data2 << 7) | data1;
                g_controllers.values_[kControllerPitch] = bend;
            }
            break;
    }
}

static void plugin_set_param(const char *key, const char *val) {
    if (strcmp(key, "syx_path") == 0) {
        load_syx(val);
        if (g_preset_count > 0) {
            select_preset(0);
        }
    } else if (strcmp(key, "preset") == 0) {
        select_preset(atoi(val));
    } else if (strcmp(key, "octave_transpose") == 0) {
        g_octave_transpose = atoi(val);
        if (g_octave_transpose < -4) g_octave_transpose = -4;
        if (g_octave_transpose > 4) g_octave_transpose = 4;
    } else if (strcmp(key, "output_level") == 0) {
        g_output_level = atoi(val);
        if (g_output_level < 0) g_output_level = 0;
        if (g_output_level > 100) g_output_level = 100;
    } else if (strcmp(key, "all_notes_off") == 0) {
        all_notes_off();  /* Trigger release phase */
    } else if (strcmp(key, "panic") == 0) {
        panic();  /* Full reset - immediately silence */
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "patch_name") == 0 || strcmp(key, "preset_name") == 0) {
        strncpy(buf, g_patch_name, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "syx_path") == 0) {
        strncpy(buf, g_patch_path, buf_len - 1);
        return strlen(buf);
    } else if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", g_current_preset);
    } else if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", g_preset_count);
    } else if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "%d", g_active_voices);
    } else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", g_octave_transpose);
    } else if (strcmp(key, "algorithm") == 0) {
        return snprintf(buf, buf_len, "%d", g_current_patch[134] + 1);
    } else if (strcmp(key, "output_level") == 0) {
        return snprintf(buf, buf_len, "%d", g_output_level);
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    /* Clear output */
    memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));

    /* Process in N-sample blocks (N=64 from synth.h) */
    int remaining = frames;
    int out_pos = 0;

    while (remaining > 0) {
        int block_size = (remaining > N) ? N : remaining;

        /* Clear render buffer */
        memset(g_render_buffer, 0, sizeof(g_render_buffer));

        /* Update LFO - don't call keydown() here, it's only for note starts */
        int32_t lfo_val = g_lfo.getsample();
        int32_t lfo_delay = g_lfo.getdelay();

        /* Count active voices and render */
        g_active_voices = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (g_voice_note[v] >= 0 || g_voices[v]->isPlaying()) {
                g_voices[v]->compute(g_render_buffer, lfo_val, lfo_delay, &g_controllers);

                if (!g_voices[v]->isPlaying()) {
                    g_voice_note[v] = -1;  /* Voice finished */
                } else {
                    g_active_voices++;
                }
            }
        }

        /* Convert to stereo int16 output */
        for (int i = 0; i < block_size; i++) {
            /* msfa outputs 32-bit samples in a specific format.
             * Dexed uses: val >> 4, then clip to 24-bit, then >> 9 */
            int32_t val = g_render_buffer[i] >> 4;

            /* Apply output level scaling (0-100 -> 0.0-1.0) */
            val = (val * g_output_level) / 100;

            /* Clip to 24-bit range and shift to 16-bit */
            int16_t sample;
            if (val < -(1 << 24)) {
                sample = -32768;
            } else if (val >= (1 << 24)) {
                sample = 32767;
            } else {
                sample = (int16_t)(val >> 9);
            }

            /* Mono to stereo */
            out_interleaved_lr[out_pos * 2] = sample;
            out_interleaved_lr[out_pos * 2 + 1] = sample;
            out_pos++;
        }

        remaining -= block_size;
    }
}

/* === Plugin entry point === */

extern "C" plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;

    if (host->api_version != MOVE_PLUGIN_API_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "API version mismatch: host=%d, plugin=%d",
                 host->api_version, MOVE_PLUGIN_API_VERSION);
        if (host->log) host->log(msg);
        return NULL;
    }

    memset(&g_plugin_api, 0, sizeof(g_plugin_api));
    g_plugin_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_plugin_api.on_load = plugin_on_load;
    g_plugin_api.on_unload = plugin_on_unload;
    g_plugin_api.on_midi = plugin_on_midi;
    g_plugin_api.set_param = plugin_set_param;
    g_plugin_api.get_param = plugin_get_param;
    g_plugin_api.render_block = plugin_render_block;

    plugin_log("DX7 plugin initialized");

    return &g_plugin_api;
}

/* ========================================================================
 * PLUGIN API V2 - INSTANCE-BASED (for multi-instance support)
 * ======================================================================== */

/* v2 instance structure */
typedef struct {
    /* Module path */
    char module_dir[512];

    /* Preset state */
    int current_preset;
    int preset_count;
    int octave_transpose;
    char patch_path[512];
    char patch_name[128];
    int active_voices;
    int output_level;

    /* Tuning */
    std::shared_ptr<TuningState> tuning;

    /* Controllers */
    Controllers controllers;

    /* FM core and LFO */
    FmCore fm_core;
    Lfo lfo;

    /* Voices */
    Dx7Note* voices[MAX_VOICES];
    int voice_note[MAX_VOICES];
    int voice_age[MAX_VOICES];
    bool voice_sustained[MAX_VOICES];
    int age_counter;
    bool sustain_pedal;

    /* Patches */
    uint8_t current_patch[DX7_PATCH_SIZE];
    uint8_t patches[MAX_PATCHES][DX7_PATCH_SIZE];
    char patch_names[MAX_PATCHES][11];

    /* Render buffer */
    int32_t render_buffer[N];
} dx7_instance_t;

/* v2: Logging helper */
static void v2_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[dx7 v2] %s", msg);
        g_host->log(buf);
    }
}

/* v2: Initialize default patch */
static void v2_init_default_patch(dx7_instance_t *inst) {
    memset(inst->current_patch, 0, DX7_PATCH_SIZE);

    /* Set up a simple init patch - operator 6 only with a sine wave */
    for (int op = 0; op < 6; op++) {
        int base = op * 21;
        /* EG rates */
        inst->current_patch[base + 0] = 99;  /* R1 */
        inst->current_patch[base + 1] = 99;  /* R2 */
        inst->current_patch[base + 2] = 99;  /* R3 */
        inst->current_patch[base + 3] = 99;  /* R4 */
        /* EG levels */
        inst->current_patch[base + 4] = 99;  /* L1 */
        inst->current_patch[base + 5] = 99;  /* L2 */
        inst->current_patch[base + 6] = 99;  /* L3 */
        inst->current_patch[base + 7] = 0;   /* L4 */
        /* Other params */
        inst->current_patch[base + 17] = (op == 5) ? 99 : 0;  /* Output level - only op6 on */
        inst->current_patch[base + 20] = 1;  /* Oscillator mode = ratio */
    }

    /* Algorithm = 1 */
    inst->current_patch[134] = 0;
    /* Feedback = 0 */
    inst->current_patch[135] = 0;
    /* LFO settings */
    inst->current_patch[137] = 35;  /* LFO speed */
    inst->current_patch[138] = 0;   /* LFO delay */
    inst->current_patch[139] = 0;   /* LFO PMD */
    inst->current_patch[140] = 0;   /* LFO AMD */
    inst->current_patch[143] = 24;  /* Transpose */

    strncpy(inst->patch_name, "Init", sizeof(inst->patch_name) - 1);
}

/* v2: Load syx file into instance */
static int v2_load_syx(dx7_instance_t *inst, const char *path) {
    char msg[256];

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof(msg), "v2: Cannot open syx: %s", path);
        v2_log(msg);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Standard DX7 32-voice sysex is 4104 bytes */
    if (size != 4104) {
        snprintf(msg, sizeof(msg), "v2: Invalid syx size: %ld (expected 4104)", size);
        v2_log(msg);
        fclose(f);
        return -1;
    }

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        fclose(f);
        return -1;
    }

    fread(data, 1, size, f);
    fclose(f);

    /* Verify sysex header: F0 43 00 09 20 00 */
    if (data[0] != 0xF0 || data[1] != 0x43 || data[3] != 0x09) {
        v2_log("v2: Invalid DX7 sysex header");
        free(data);
        return -1;
    }

    /* Extract 32 patches starting at offset 6 */
    inst->preset_count = 32;
    for (int i = 0; i < 32; i++) {
        uint8_t *packed = &data[6 + i * 128];
        unpack_patch(packed, inst->patches[i]);

        /* Extract name */
        for (int j = 0; j < 10; j++) {
            char c = inst->patches[i][145 + j];
            inst->patch_names[i][j] = (c >= 32 && c < 127) ? c : ' ';
        }
        inst->patch_names[i][10] = '\0';
    }

    free(data);
    strncpy(inst->patch_path, path, sizeof(inst->patch_path) - 1);

    snprintf(msg, sizeof(msg), "v2: Loaded 32 patches from: %s", path);
    v2_log(msg);

    return 0;
}

/* v2: Select preset by index */
static void v2_select_preset(dx7_instance_t *inst, int index) {
    if (index < 0) index = inst->preset_count - 1;
    if (index >= inst->preset_count) index = 0;

    inst->current_preset = index;
    memcpy(inst->current_patch, inst->patches[index], DX7_PATCH_SIZE);
    strncpy(inst->patch_name, inst->patch_names[index], sizeof(inst->patch_name) - 1);

    /* Update LFO for new patch */
    inst->lfo.reset(inst->current_patch + 137);

    char msg[128];
    snprintf(msg, sizeof(msg), "v2: Preset %d: %s (alg %d)",
             index, inst->patch_name, inst->current_patch[134] + 1);
    v2_log(msg);
}

/* v2: Allocate a voice using voice stealing */
static int v2_allocate_voice(dx7_instance_t *inst) {
    /* First try to find a free voice */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (inst->voice_note[i] < 0) {
            return i;
        }
    }

    /* No free voice, steal the oldest one */
    int oldest = 0;
    int oldest_age = inst->voice_age[0];
    for (int i = 1; i < MAX_VOICES; i++) {
        if (inst->voice_age[i] < oldest_age) {
            oldest = i;
            oldest_age = inst->voice_age[i];
        }
    }

    /* Voice already exists (was allocated at create_instance), just reuse */
    return oldest;
}

/* v2: Create instance */
static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    dx7_instance_t *inst = new dx7_instance_t();
    if (!inst) {
        fprintf(stderr, "DX7 v2: Failed to allocate instance\n");
        return NULL;
    }

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    inst->current_preset = 0;
    inst->preset_count = 1;  /* At least init patch */
    inst->octave_transpose = 0;
    inst->active_voices = 0;
    inst->output_level = 50;
    inst->age_counter = 0;
    inst->sustain_pedal = false;
    strncpy(inst->patch_name, "Init", sizeof(inst->patch_name) - 1);

    /* Initialize tuning */
    inst->tuning = std::make_shared<TuningState>();

    /* Initialize controllers */
    inst->controllers.core = &inst->fm_core;
    inst->controllers.masterTune = 0;
    memset(inst->controllers.values_, 0, sizeof(inst->controllers.values_));
    inst->controllers.values_[kControllerPitch] = 0x2000;  /* Center pitch bend */
    inst->controllers.values_[kControllerPitchRangeUp] = 2;
    inst->controllers.values_[kControllerPitchRangeDn] = 2;
    inst->controllers.values_[kControllerPitchStep] = 0;
    inst->controllers.modwheel_cc = 0;
    inst->controllers.breath_cc = 0;
    inst->controllers.foot_cc = 0;
    inst->controllers.aftertouch_cc = 0;
    inst->controllers.portamento_cc = 0;
    inst->controllers.portamento_enable_cc = false;
    inst->controllers.portamento_gliss_cc = false;
    inst->controllers.mpeEnabled = false;

    /* Configure modulation sources */
    inst->controllers.wheel.range = 99;
    inst->controllers.wheel.pitch = true;
    inst->controllers.wheel.amp = true;
    inst->controllers.wheel.eg = false;

    inst->controllers.at.range = 99;
    inst->controllers.at.pitch = true;
    inst->controllers.at.amp = true;
    inst->controllers.at.eg = false;

    inst->controllers.breath.range = 99;
    inst->controllers.breath.pitch = false;
    inst->controllers.breath.amp = true;
    inst->controllers.breath.eg = false;

    inst->controllers.foot.range = 99;
    inst->controllers.foot.pitch = false;
    inst->controllers.foot.amp = false;
    inst->controllers.foot.eg = false;

    inst->controllers.refresh();

    /* Initialize tables (global - safe to call multiple times) */
    Exp2::init();
    Sin::init();
    Freqlut::init(MOVE_SAMPLE_RATE);
    PitchEnv::init(MOVE_SAMPLE_RATE);
    Env::init_sr(MOVE_SAMPLE_RATE);
    Porta::init_sr(MOVE_SAMPLE_RATE);

    /* Initialize voices */
    for (int i = 0; i < MAX_VOICES; i++) {
        inst->voices[i] = new Dx7Note(inst->tuning, nullptr);
        inst->voice_note[i] = -1;
        inst->voice_age[i] = 0;
        inst->voice_sustained[i] = false;
    }

    /* Initialize default patch */
    v2_init_default_patch(inst);
    memcpy(inst->patches[0], inst->current_patch, DX7_PATCH_SIZE);
    strcpy(inst->patch_names[0], "Init");

    /* Initialize LFO */
    inst->lfo.reset(inst->current_patch + 137);

    /* Try to load syx_path from json_defaults */
    char syx_path[512] = {0};
    if (json_defaults) {
        const char *pos = strstr(json_defaults, "\"syx_path\"");
        if (pos) {
            pos = strchr(pos, ':');
            if (pos) {
                pos = strchr(pos, '"');
                if (pos) {
                    pos++;
                    int i = 0;
                    while (*pos && *pos != '"' && i < (int)sizeof(syx_path) - 1) {
                        syx_path[i++] = *pos++;
                    }
                    syx_path[i] = '\0';
                }
            }
        }
    }

    /* Load syx file */
    if (syx_path[0]) {
        v2_load_syx(inst, syx_path);
    } else {
        /* Try default patches.syx in module dir */
        char default_syx[512];
        snprintf(default_syx, sizeof(default_syx), "%s/patches.syx", module_dir);
        v2_load_syx(inst, default_syx);
    }

    /* Select first preset if we have patches */
    if (inst->preset_count > 0) {
        v2_select_preset(inst, 0);
    }

    v2_log("Instance created");
    return inst;
}

/* v2: Destroy instance */
static void v2_destroy_instance(void *instance) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst) return;

    /* Clean up voices */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (inst->voices[i]) {
            delete inst->voices[i];
            inst->voices[i] = NULL;
        }
    }

    v2_log("Instance destroyed");
    delete inst;
}

/* v2: MIDI handler */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst || len < 1) return;
    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = (len > 1) ? msg[1] : 0;
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    switch (status) {
        case 0x90: /* Note On */
            if (data2 > 0) {
                int note = data1 + (inst->octave_transpose * 12);
                if (note < 0) note = 0;
                if (note > 127) note = 127;

                /* Count active voices before adding */
                int active_before = 0;
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (inst->voice_note[i] >= 0) active_before++;
                }

                int voice = v2_allocate_voice(inst);
                inst->voices[voice]->init(inst->current_patch, note, data2, 0, &inst->controllers);
                inst->voice_note[voice] = note;
                inst->voice_age[voice] = inst->age_counter++;
                inst->voice_sustained[voice] = false;

                /* Only trigger LFO sync on first voice */
                if (active_before == 0) {
                    inst->lfo.keydown();
                }
            } else {
                /* Note off via velocity 0 */
                int note = data1 + (inst->octave_transpose * 12);
                if (note < 0) note = 0;
                if (note > 127) note = 127;

                for (int i = 0; i < MAX_VOICES; i++) {
                    if (inst->voice_note[i] == note) {
                        if (inst->sustain_pedal) {
                            inst->voice_sustained[i] = true;
                        } else if (inst->voices[i]) {
                            inst->voices[i]->keyup();
                        }
                    }
                }
            }
            break;

        case 0x80: /* Note Off */
            {
                int note = data1 + (inst->octave_transpose * 12);
                if (note < 0) note = 0;
                if (note > 127) note = 127;

                for (int i = 0; i < MAX_VOICES; i++) {
                    if (inst->voice_note[i] == note) {
                        if (inst->sustain_pedal) {
                            inst->voice_sustained[i] = true;
                        } else if (inst->voices[i]) {
                            inst->voices[i]->keyup();
                        }
                    }
                }
            }
            break;

        case 0xB0: /* Control Change */
            if (data1 == 64) { /* Sustain pedal */
                inst->sustain_pedal = (data2 >= 64);
                if (!inst->sustain_pedal) {
                    /* Release sustained notes */
                    for (int i = 0; i < MAX_VOICES; i++) {
                        if (inst->voice_sustained[i] && inst->voices[i]) {
                            inst->voices[i]->keyup();
                            inst->voice_sustained[i] = false;
                        }
                    }
                }
            } else if (data1 == 1) { /* Mod wheel */
                inst->controllers.modwheel_cc = data2;
            } else if (data1 == 123) { /* All notes off */
                for (int i = 0; i < MAX_VOICES; i++) {
                    inst->voice_note[i] = -1;
                    inst->voice_sustained[i] = false;
                }
                inst->active_voices = 0;
            }
            break;

        case 0xE0: /* Pitch bend */
            {
                int bend = ((data2 << 7) | data1);
                inst->controllers.values_[kControllerPitch] = bend;
            }
            break;
    }
}

/* v2: Set parameter */
static void v2_set_param(void *instance, const char *key, const char *val) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst) return;

    if (strcmp(key, "syx_path") == 0) {
        v2_load_syx(inst, val);
        if (inst->preset_count > 0) {
            v2_select_preset(inst, 0);
        }
    } else if (strcmp(key, "preset") == 0) {
        v2_select_preset(inst, atoi(val));
    } else if (strcmp(key, "octave_transpose") == 0) {
        int v = atoi(val);
        if (v < -3) v = -3;
        if (v > 3) v = 3;
        inst->octave_transpose = v;
    } else if (strcmp(key, "output_level") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        inst->output_level = v;
    } else if (strcmp(key, "panic") == 0 || strcmp(key, "all_notes_off") == 0) {
        /* Silence all voices */
        for (int i = 0; i < MAX_VOICES; i++) {
            if (inst->voices[i]) {
                delete inst->voices[i];
                inst->voices[i] = new Dx7Note(inst->tuning, nullptr);
            }
            inst->voice_note[i] = -1;
            inst->voice_sustained[i] = false;
            inst->voice_age[i] = 0;
        }
        inst->sustain_pedal = false;
        inst->active_voices = 0;
    }
}

/* v2: Get parameter */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "%s", inst->patch_name);
    }
    if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    }
    if (strcmp(key, "current_preset") == 0 || strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    }
    if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    }
    if (strcmp(key, "active_voices") == 0) {
        return snprintf(buf, buf_len, "%d", inst->active_voices);
    }
    if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "%d", MAX_VOICES);
    }

    return -1;
}

/* v2: Render block */
static void v2_render_block(void *instance, int16_t *out, int frames) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Clear output */
    memset(out, 0, frames * 2 * sizeof(int16_t));

    /* Process in N-sample blocks */
    int remaining = frames;
    int out_pos = 0;

    while (remaining > 0) {
        int block_size = (remaining > N) ? N : remaining;

        /* Clear render buffer */
        memset(inst->render_buffer, 0, sizeof(inst->render_buffer));

        /* Get LFO values */
        int32_t lfo_val = inst->lfo.getsample();
        int32_t lfo_delay = inst->lfo.getdelay();

        /* Count active voices and render */
        inst->active_voices = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (inst->voice_note[v] >= 0 || inst->voices[v]->isPlaying()) {
                inst->voices[v]->compute(inst->render_buffer, lfo_val, lfo_delay, &inst->controllers);

                if (!inst->voices[v]->isPlaying()) {
                    inst->voice_note[v] = -1;  /* Voice finished */
                } else {
                    inst->active_voices++;
                }
            }
        }

        /* Convert to stereo int16 output */
        for (int i = 0; i < block_size; i++) {
            int32_t val = inst->render_buffer[i] >> 4;
            val = (val * inst->output_level) / 100;

            int16_t sample;
            if (val < -(1 << 24)) {
                sample = -32768;
            } else if (val >= (1 << 24)) {
                sample = 32767;
            } else {
                sample = (int16_t)(val >> 9);
            }

            out[out_pos * 2] = sample;
            out[out_pos * 2 + 1] = sample;
            out_pos++;
        }

        remaining -= block_size;
    }
}

/* v2 API struct */
static plugin_api_v2_t g_plugin_api_v2;

/* v2 Entry Point */
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.render_block = v2_render_block;

    v2_log("v2 API initialized");

    return &g_plugin_api_v2;
}
