/*
 * DX7 Synth Module UI
 *
 * Uses shared sound generator UI base with custom algorithm display.
 */

import { createSoundGeneratorUI } from '../../shared/sound_generator_ui.mjs';

/* Algorithm state */
let algorithm = 1;

/* Create the UI with DX7-specific customizations */
const ui = createSoundGeneratorUI({
    moduleName: 'DX7',

    onInit: (state) => {
        /* Get initial algorithm from DSP */
        const alg = host_module_get_param('algorithm');
        if (alg) algorithm = parseInt(alg) || 1;
    },

    onTick: (state) => {
        /* Update algorithm from DSP */
        const alg = host_module_get_param('algorithm');
        if (alg) {
            const newAlg = parseInt(alg) || 1;
            if (newAlg !== algorithm) {
                algorithm = newAlg;
                ui.requestRedraw();
            }
        }
    },

    /* Draw algorithm info between preset name and status bar */
    drawCustom: (y, state) => {
        print(2, y, `Algorithm: ${algorithm}`, 1);
        return y + 12;
    },

    onPresetChange: (preset) => {
        /* Use panic for full reset on preset change (DX7 specific) */
        host_module_set_param('panic', '1');

        /* Update algorithm after preset change */
        const alg = host_module_get_param('algorithm');
        if (alg) algorithm = parseInt(alg) || 1;
    },

    showPolyphony: true,
    showOctave: true,
});

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
