/*  KallistiOS ##version##

    rumble.c
    Copyright (C) 2004 SinisterTengu
    Copyright (C) 2008, 2023, 2025 Donald Haase
    Copyright (C) 2024, 2025 Daniel Fairchild

*/

/*
        This example allows you to send raw commands to the rumble accessory
   (aka purupuru).

        This is a recreation of an original posted by SinisterTengu in 2004
   here: https://dcemulation.org/phpBB/viewtopic.php?p=490067#p490067 .
   Unfortunately, that one is lost, but I had based my vmu_beep testing on it,
   and the principle is the same. In each, a single 32-bit value is sent to the
   device which defines the features of the rumbling.

        TODO: This should be updated at some point to display and work from the
   macros in dc/maple/purupuru.h that define the characteristics of the raw
   32-bit value.

 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <kos/init.h>

#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/purupuru.h>

#include <dc/minifont.h>
#include <dc/video.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

typedef struct {
    purupuru_effect_t effect;
    const char *description;
} baked_pattern_t;

static const baked_pattern_t catalog[] = {
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .freq = 26, .inc =  1}, .description = "Basic Thud (simple .5s jolt)"},
    {.effect = {.cont = true,  .motor = 1, .fpow = 1, .freq =  7, .inc = 49}, .description = "Car Idle (69 Mustang)"},
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .conv = true, .freq = 21, .inc = 38}, .description = "Car Idle (VW beetle)"},
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .conv = true, .freq = 57, .inc = 51}, .description = "Eathquake (Vibrate, and fade out)"},
    {.effect = {.cont = true,  .motor = 1, .fpow = 1, .freq =  40, .inc = 5}, .description = "Helicopter"},
    {.effect = {.cont = false, .motor = 1, .fpow = 2, .freq =  7, .inc = 0}, .description = "Ship's Thrust (as in AAC)"},
};

/* motor cannot be 0 (will generate error on official hardware), but we can set
 * everything else to 0 for stopping */
static const purupuru_effect_t rumble_stop = {.motor = 1};
static purupuru_effect_t effect = rumble_stop;

static uint8_t n[8];
static int cursor_pos = 0;
static size_t catalog_index = 0;

static inline void word2hexbytes(uint32_t word, uint8_t *bytes) {
    for (int i = 0; i < 8; i++) {
        bytes[i] = (word >> (28 - (i * 4))) & 0xf;
    }
}
static inline void hexbytes2word(const uint8_t *bytes, uint32_t *word) {
    *word = 0;

    for (int i = 0; i < 8; i++) {
        *word |= (bytes[i] & 0xf) << (28 - (i * 4));
    }
}


void print_rumble_fields(purupuru_effect_t fields) {
    printf("Rumble Fields:\n");
    printf("  .cont   =  %s,\n", fields.cont ? "true" : "false");
    printf("  .motor  =  %u,\n", fields.motor);

    printf("  .bpow   =  %u,\n", fields.bpow);
    printf("  .fpow   =  %u,\n", fields.fpow);
    printf("  .div    =  %s,\n", fields.div ? "true" : "false");
    printf("  .conv   =  %s,\n", fields.conv ? "true" : "false");

    printf("  .freq   =  %u,\n", fields.freq);
    printf("  .inc    =  %u,\n", fields.inc);
}

void redraw_screen() {
#define STRBUFSIZE 64
    char str_buffer[STRBUFSIZE];
    int textpos_x = 128, textpos_y = 32;

    /* Start drawing and draw the header */
    vid_clear(0, 0, 0);
    minifont_set_color(0xff, 0xc0, 0x10); /* gold */
    minifont_draw_str(vram_s + (640 * textpos_y) + textpos_x, 640,
                      "Rumble Accessory Tester");

    textpos_y += 20;
    textpos_x = 10;
    minifont_draw_str(vram_s + (640 * textpos_y) + textpos_x, 640,
                      "effect hex value:");
    minifont_set_color(255, 0, 255); /* Magenta */
    snprintf(str_buffer, STRBUFSIZE, "0x%08lx", effect.raw);
    minifont_draw_str(vram_s + (640 * textpos_y) + textpos_x + 145, 640,
                      str_buffer);

    /* Draw the bottom half of the screen and finish it up. */
    textpos_y = 360;
    textpos_x = 10;
    minifont_set_color(255, 255, 255); /* White */
    const char *instructions[] = {"Press left/right to switch field.",
                                  "Press up/down to change values.",
                                  "Press A to send effect to rumblepack.",
                                  "Press B to stop rumble.",
                                  "Press X for next baked pattern",
                                  "Press Start to quit."
                                 };

    for (size_t i = 0; i < sizeof(instructions) / sizeof(instructions[0]);
         i++) {
        minifont_draw_str(vram_s + (640 * textpos_y) + textpos_x, 640,
                          instructions[i]);
        textpos_y += 16;
    }

    vid_flip(-1);
}

/* This blocks waiting for a specified device to be present and valid */
void wait_for_dev_attach(maple_device_t **dev_ptr, unsigned int func) {
    maple_device_t *dev = NULL;

    do {
        /* If we already have it, and it's still valid, leave */
        for(int u = 0; u < MAPLE_UNIT_COUNT; u++) {
            /* Only check for valid device on port A/0 */
            dev = maple_enum_dev(0, u);

            if(dev != NULL && dev->valid && (dev->info.functions & func)) {
                *dev_ptr = dev;
                return;
            }
        }
    }
    while((dev == NULL) || !dev->valid);

    /* Draw up a screen */
    vid_clear(0, 0, 0);
    int textpos_x = 40, textpos_y = 200;

    switch (func) {
        case MAPLE_FUNC_CONTROLLER:
            minifont_draw_str(vram_s + (640 * textpos_y) + textpos_x, 640,
                              "Please attach a controller to port A!");
            break;

        case MAPLE_FUNC_PURUPURU:
            minifont_draw_str(
                vram_s + (640 * textpos_y) + textpos_x, 640,
                "Please attach a rumbler to controller in port A!");

        default:
            break;
    }

    vid_flip(-1);
}


int main(int argc, char *argv[]) {
    cont_state_t *state;
    maple_device_t *contdev = NULL, *purudev = NULL;

    uint16_t old_buttons = 0, rel_buttons = 0;

    word2hexbytes(effect.raw, n);
    vid_set_mode(DM_640x480 | DM_MULTIBUFFER, PM_RGB565);

    /* Loop until Start is pressed */
    while (!(rel_buttons & CONT_START)) {
        /* Before drawing the screen, trap into these functions to be
           sure that there's at least one controller and one rumbler */
        wait_for_dev_attach(&contdev, MAPLE_FUNC_CONTROLLER);
        wait_for_dev_attach(&purudev, MAPLE_FUNC_PURUPURU);

        /* Store current button states + buttons which have been released. */
        state = (cont_state_t *)maple_dev_status(contdev);

        /* Make sure we can rely on the state, otherwise loop. */
        if (state == NULL) continue;

        rel_buttons = (old_buttons ^ state->buttons);

        if ((state->buttons & CONT_DPAD_LEFT) &&
            (rel_buttons & CONT_DPAD_LEFT)) {
            if (cursor_pos > 0) cursor_pos--;
        }

        if ((state->buttons & CONT_DPAD_RIGHT) &&
            (rel_buttons & CONT_DPAD_RIGHT)) {
            if (cursor_pos < 7) cursor_pos++;
        }

        if ((state->buttons & CONT_DPAD_UP) && (rel_buttons & CONT_DPAD_UP)) {
            if (n[cursor_pos] < 15) n[cursor_pos]++;
        }

        if ((state->buttons & CONT_DPAD_DOWN) &&
            (rel_buttons & CONT_DPAD_DOWN)) {
            if (n[cursor_pos] > 0) n[cursor_pos]--;
        }

        if ((state->buttons & CONT_X) && (rel_buttons & CONT_X)) {
            printf("Setting baked effect:\n\t'%s'\n",
                   catalog[catalog_index].description);
            word2hexbytes(catalog[catalog_index].effect.raw, n);
            catalog_index++;

            if (catalog_index >= sizeof(catalog) / sizeof(baked_pattern_t)) {
                catalog_index = 0;
            }
        }


        if ((state->buttons & CONT_A) && (rel_buttons & CONT_A)) {
            purupuru_rumble(purudev, &effect);
            /* We print these out to make it easier to track the options chosen
             */
            printf("Rumble: 0x%lx!\n", effect.raw);
            print_rumble_fields(effect);
        }

        if ((state->buttons & CONT_B) && (rel_buttons & CONT_B)) {
            purupuru_rumble(purudev, &rumble_stop);
            printf("Rumble Stopped!\n");
        }

        old_buttons = state->buttons;
        hexbytes2word(n, &effect.raw);
        redraw_screen();
    }

    /* Stop rumbling before exiting, if it still exists. */
    if ((purudev != NULL) && purudev->valid)
        purupuru_rumble(purudev, &rumble_stop);

    return 0;
}
