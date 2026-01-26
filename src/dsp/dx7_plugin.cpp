/*
 * Dexed Synth DSP Plugin
 *
 * Uses msfa (Music Synthesizer for Android) FM engine from Dexed.
 * Provides 6-operator FM synthesis with DX7-compatible patch support.
 *
 * V2 API only - instance-based for multi-instance support.
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

/* Plugin API v2 - Instance-based for multi-instance support */
#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
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

/* Host API reference */
static const host_api_v1_t *g_host = NULL;

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[dexed] %s", msg);
        g_host->log(buf);
    }
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

    /* Load error state */
    char load_error[256];
} dx7_instance_t;

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
        snprintf(msg, sizeof(msg), "Cannot open syx: %s", path);
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

    snprintf(msg, sizeof(msg), "Loaded 32 patches from: %s", path);
    plugin_log(msg);

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
    snprintf(msg, sizeof(msg), "Preset %d: %s (alg %d)",
             index, inst->patch_name, inst->current_patch[134] + 1);
    plugin_log(msg);
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
        fprintf(stderr, "Dexed: Failed to allocate instance\n");
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

    /* Initialize load error */
    inst->load_error[0] = '\0';

    /* Load syx file */
    int syx_result = -1;
    if (syx_path[0]) {
        syx_result = v2_load_syx(inst, syx_path);
        if (syx_result != 0) {
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "Dexed: patches.syx not found");
        }
    } else {
        /* Try default patches.syx in module dir */
        char default_syx[512];
        snprintf(default_syx, sizeof(default_syx), "%s/patches.syx", module_dir);
        syx_result = v2_load_syx(inst, default_syx);
        if (syx_result != 0) {
            snprintf(inst->load_error, sizeof(inst->load_error),
                     "Dexed: patches.syx not found");
        }
    }

    /* Select first preset if we have patches */
    if (inst->preset_count > 0) {
        v2_select_preset(inst, 0);
    }

    plugin_log("Instance created");
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

    plugin_log("Instance destroyed");
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

/* Helper to extract a JSON number value by key */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

/* v2: Set parameter */
static void v2_set_param(void *instance, const char *key, const char *val) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst) return;

    /* State restore from patch save */
    if (strcmp(key, "state") == 0) {
        float fval;

        /* Restore preset first */
        if (json_get_number(val, "preset", &fval) == 0) {
            int idx = (int)fval;
            if (idx >= 0 && idx < inst->preset_count) {
                v2_select_preset(inst, idx);
            }
        }

        /* Restore octave transpose */
        if (json_get_number(val, "octave_transpose", &fval) == 0) {
            inst->octave_transpose = (int)fval;
            if (inst->octave_transpose < -3) inst->octave_transpose = -3;
            if (inst->octave_transpose > 3) inst->octave_transpose = 3;
        }

        /* Restore output level */
        if (json_get_number(val, "output_level", &fval) == 0) {
            inst->output_level = (int)fval;
            if (inst->output_level < 0) inst->output_level = 0;
            if (inst->output_level > 100) inst->output_level = 100;
        }
        return;
    }

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

    if (strcmp(key, "load_error") == 0) {
        if (inst->load_error[0]) {
            return snprintf(buf, buf_len, "%s", inst->load_error);
        }
        return 0;  /* No error */
    }
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "%s", inst->patch_name);
    }
    if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    }
    if (strcmp(key, "current_preset") == 0 || strcmp(key, "preset") == 0 || strcmp(key, "current_patch") == 0) {
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
    /* Unified bank/preset parameters for Chain compatibility */
    if (strcmp(key, "bank_name") == 0) {
        /* Bank = syx filename (extract basename from patch_path) */
        const char *basename = strrchr(inst->patch_path, '/');
        if (basename) {
            basename++;  /* Skip the '/' */
        } else {
            basename = inst->patch_path;
        }
        /* Remove .syx extension if present */
        char name[128];
        strncpy(name, basename, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        char *ext = strrchr(name, '.');
        if (ext && (strcmp(ext, ".syx") == 0 || strcmp(ext, ".SYX") == 0)) {
            *ext = '\0';
        }
        return snprintf(buf, buf_len, "%s", name[0] ? name : "Dexed");
    }
    if (strcmp(key, "patch_in_bank") == 0) {
        /* 1-indexed position within the 32-patch syx bank */
        return snprintf(buf, buf_len, "%d", inst->current_preset + 1);
    }
    if (strcmp(key, "bank_count") == 0) {
        /* DX7 loads one syx file at a time */
        return snprintf(buf, buf_len, "1");
    }
    /* UI hierarchy for shadow parameter editor */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"params\","
                    "\"knobs\":[],"
                    "\"params\":[]"
                "},"
                "\"params\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"output_level\",\"octave_transpose\"],"
                    "\"params\":[\"output_level\",\"octave_transpose\"]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }
    /* Output level for get_param */
    if (strcmp(key, "output_level") == 0) {
        return snprintf(buf, buf_len, "%d", inst->output_level);
    }
    /* Octave transpose for get_param */
    if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    }
    /* Chain params metadata for shadow UI */
    if (strcmp(key, "chain_params") == 0) {
        const char *params_json = "["
            "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999},"
            "{\"key\":\"output_level\",\"name\":\"Output Level\",\"type\":\"int\",\"min\":0,\"max\":100},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
    }
    /* State serialization for patch save/load */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"preset\":%d,\"octave_transpose\":%d,\"output_level\":%d}",
            inst->current_preset, inst->octave_transpose, inst->output_level);
    }

    return -1;
}

/* v2: Get error message */
static int v2_get_error(void *instance, char *buf, int buf_len) {
    dx7_instance_t *inst = (dx7_instance_t*)instance;
    if (!inst || !inst->load_error[0]) {
        return 0;  /* No error */
    }
    int len = strlen(inst->load_error);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, inst->load_error, len);
    buf[len] = '\0';
    return len;
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
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;

    plugin_log("V2 API initialized");

    return &g_plugin_api_v2;
}
