/*  KallistiOS ##version##

    rumble.c
    Copyright (C) 2025 Daniel Fairchild

*/

/*
    This example allows you to send raw commands to the rumble accessory (aka
   purupuru).
*/

#include <stdint.h>
#include <stdio.h>
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

/* motor cannot be 0 (will generate error on official hardware), but we can set
 * everything else to 0 for stopping */
static const purupuru_effect_t rumble_stop = {.motor = 1};
static purupuru_effect_t effect = rumble_stop;

static size_t catalog_index = 0;
static const baked_pattern_t catalog[] = {
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .freq = 26, .inc = 1},
     .description = "Basic Thud (simple .5s jolt)"},
    {.effect = {.cont = true, .motor = 1, .fpow = 1, .freq = 7, .inc = 49},
     .description = "Car Idle (69 Mustang)"},
    {.effect = {.cont = false,
                .motor = 1,
                .fpow = 7,
                .conv = true,
                .freq = 21,
                .inc = 38},
     .description = "Car Idle (VW beetle)"},
    {.effect = {.cont = false,
                .motor = 1,
                .fpow = 7,
                .conv = true,
                .freq = 57,
                .inc = 51},
     .description = "Eathquake (Vibrate, and fade out)"},
    {.effect = {.cont = true, .motor = 1, .fpow = 1, .freq = 40, .inc = 5},
     .description = "Helicopter"},
    {.effect = {.cont = false, .motor = 1, .fpow = 2, .freq = 7, .inc = 0},
     .description = "Ship's Thrust (as in AAC)"},
};

static inline void word2hexbytes(uint32_t word, uint8_t *bytes) {
  for (int i = 0; i < 8; i++) {
    bytes[i] = (word >> (28 - (i * 4))) & 0xf;
  }
}

static int cursor_pos = 0;
static uint16_t old_buttons = 0, rel_buttons = 0;

static const char *fieldnames[] = {"cont", "res",  "motor", "bpow", "div",
                                   "fpow", "conv", "freq",  "inc"};

static inline uint8_t offset2field(int offset) {
  switch (offset) {
  case 0:
    return effect.cont; // cont
  case 1:
    return effect.res; // res
  case 2:
    return effect.motor; // motor
  case 3:
    return effect.bpow; // bpow
  case 4:
    return effect.div; // div
  case 5:
    return effect.fpow; // fpow
  case 6:
    return effect.conv; // conv
  case 7:
    return effect.freq; // freq
  case 8:
    return effect.inc; // inc
  default:
    return -1;
  }
}
static const int num_fields = sizeof(fieldnames) / sizeof(fieldnames[0]);

static inline void alterFieldByOffset(int offset, int delta) {
  switch (offset) {
  case 0:
    effect.cont = !effect.cont; // cont
    break;
  case 1:
    effect.res = 0; // res
    break;
  case 2:
    effect.motor = (effect.motor + delta) & 0xf; // motor
    if (effect.motor == 0)
      effect.motor = 1; // motor cannot be zero
    break;
  case 3:
    effect.bpow = (effect.bpow + delta) & 0x7; // bpow
    break;
  case 4:
    effect.div = !effect.div; // div
    break;
  case 5:
    effect.fpow = (effect.fpow + delta) & 0x7; // fpow
    break;
  case 6:
    effect.conv = !effect.conv; // conv
    break;
  case 7:
    effect.freq = (effect.freq + delta) & 0xff; // freq
    break;
  case 8:
    effect.inc = (effect.inc + delta) & 0xff; // inc
    break;
  default:
    break;
  }
}

void print_rumble_fields(purupuru_effect_t fields) {
  printf("Rumble Fields:\n");
  printf("-- 1st byte ---------------------------\n");
  printf("  .cont  (0)   =  %s,\n", fields.cont ? "true" : "false");
  printf("  .res   (1-3)   =  %u,\n", fields.res);
  printf("  .motor (4-7)  =  %u,\n", fields.motor);
  printf("-- 2nd byte ---------------------------\n");
  printf("  .bpow  (0-2)  =  %u,\n", fields.bpow);
  printf("  .div   (3)    =  %s,\n", fields.div ? "true" : "false");
  printf("  .fpow  (4-6)   =  %u,\n", fields.fpow);
  printf("  .conv  (7)    =  %s,\n", fields.conv ? "true" : "false");
  printf("-- 3rd byte ---------------------------\n");
  printf("  .freq  (0-7)  =  %u,\n", fields.freq);
  printf("-- 4th byte ---------------------------\n");
  printf("  .inc   (0-7)   =  %u,\n", fields.inc);
  printf("-------------------------------------\n");

  unsigned char cmdbits[9 * 4] = {0};
  for (int bidx = 0; bidx < 4; bidx++) {
    for (int j = 0; j < 8; j++) {
      cmdbits[bidx * 9 + (7 - j)] = '0' + ((fields.raw >> (bidx * 8 + j)) & 1);
    }
    cmdbits[(3 - bidx) * 9 + 8] = '\n';
  }
  cmdbits[9 * 4 - 1] = 0;
  printf("  four byte pattern:\n%s\n", cmdbits);
  printf("-------------------------------------\n");
}

void redraw_screen() {
  vid_clear(0, 0, 0);
  int xpos = 64, ypos = 32;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, "Rumble Editor");

  /* Start drawing the changeable section of the screen */
  ypos += 30;
  xpos = 10;

  minifont_set_color(0, 0, 255); /* Blue */
  for (int i = 0; i < num_fields; i++) {
    minifont_draw_str(vram_s + (640 * ypos) + (xpos + 60 * i), 640,
                      fieldnames[i]);
  }
  ypos += 16;
  for (int i = 0; i < num_fields; i++) {
    if (cursor_pos == i)
      minifont_set_color(255, 255, 0); /* Yellow */
    else
      minifont_set_color(255, 255, 255); /* White */

    char buf[16];
    sprintf(buf, " %u ", offset2field(i));
    minifont_draw_str(vram_s + (640 * ypos) + (xpos + (60 * i)), 640, buf);
  }
  ypos += 20;
  xpos = 10;
  minifont_set_color(255, 0, 255); /* Magenta */
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, "hex value: 0x");
  xpos += 106;

  char sb[9] = {0};
  sprintf(sb, "%08x", effect.raw);
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, sb);

  /* Draw the bottom half of the screen and finish it up. */
  minifont_set_color(255, 255, 255); /* White */
  xpos = 10.0f;
  ypos += 50.0f;

  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press left/right to switch digits.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press up/down to change values.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press A to start rumblin.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press B to stop rumblin.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press X for next baked pattern");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, "Press Start to quit.");

  ypos += 16;
  vid_flip(-1);
}

/* This blocks waiting for a specified device to be present and valid */
void wait_for_dev_attach(maple_device_t **dev_ptr, unsigned int func) {
  maple_device_t *dev = *dev_ptr;

  /* If we already have it, and it's still valid, leave */
  /* dev->valid is set to false by the driver if the device
  is detached, but dev will stay not-null */
  if ((dev != NULL) && dev->valid)
    return;

  vid_clear(0, 0, 0);
  int xpos = 40, ypos = 200;
  /* Draw up a screen */
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Please attach a controller!");
  if (func == MAPLE_FUNC_CONTROLLER)
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Please attach a controller!");
  else if (func == MAPLE_FUNC_PURUPURU)
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Please attach a rumbler!");
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, "Press Start to quit.");
  vid_flip(-1);

  /* Repeatedly check until we find one and it's valid */
  while ((dev == NULL) || !dev->valid) {
    *dev_ptr = maple_enum_type(0, func);
    dev = *dev_ptr;
    usleep(100);
  }
  redraw_screen();
}

int main(int argc, char *argv[]) {
  cont_state_t *state;
  maple_device_t *contdev = NULL, *purudev = NULL;


  /* Set the video mode */
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
    if (state == NULL)
      continue;

    rel_buttons = (old_buttons ^ state->buttons);

    if ((state->buttons & CONT_DPAD_LEFT) && (rel_buttons & CONT_DPAD_LEFT)) {
      if (cursor_pos > 0) {
        cursor_pos--;
      } else {
        cursor_pos = 8;
      }
      redraw_screen();
    }

    if ((state->buttons & CONT_DPAD_RIGHT) && (rel_buttons & CONT_DPAD_RIGHT)) {
      if (cursor_pos < 8) {
        cursor_pos++;
      } else {
        cursor_pos = 0;
      }
      redraw_screen();
    }

    if ((state->buttons & CONT_DPAD_UP) && (rel_buttons & CONT_DPAD_UP)) {
      alterFieldByOffset(cursor_pos, 1);
      redraw_screen();
    }

    if ((state->buttons & CONT_DPAD_DOWN) && (rel_buttons & CONT_DPAD_DOWN)) {
      alterFieldByOffset(cursor_pos, -1);
      redraw_screen();
    }

    if ((state->buttons & CONT_X) && (rel_buttons & CONT_X)) {
      printf("Setting baked effect:\n\t'%s'\n",
             catalog[catalog_index].description);
      effect = catalog[catalog_index].effect;

      catalog_index++;
      if (catalog_index >= sizeof(catalog) / sizeof(baked_pattern_t)) {
        catalog_index = 0;
      }
      redraw_screen();
    }

    if ((state->buttons & CONT_A) && (rel_buttons & CONT_A)) {
      /* We print these out to make it easier to track the options chosen */
      printf("Rumble: 0x%lx!\n", effect.raw);
      print_rumble_fields(effect);
      purupuru_rumble(purudev, &effect);
    }

    if ((state->buttons & CONT_B) && (rel_buttons & CONT_B)) {
      purupuru_rumble(purudev, &rumble_stop);
      printf("Rumble Stopped!\n");
    }

    old_buttons = state->buttons;
  }

  /* Stop rumbling before exiting, if it still exists. */
  if ((purudev != NULL) && purudev->valid)
    purupuru_rumble(purudev, &rumble_stop);

  return 0;
}
