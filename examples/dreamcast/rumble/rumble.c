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
#include <string.h>
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
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .freq = 26, .inc = 1},
     .description = "Basic Thud (simple .5s jolt)"},
    {.effect = {.cont = true, .motor = 1, .fpow = 1, .freq = 7, .inc = 49},
     .description = "Car Idle (69 Mustang)"},
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .conv = true, .freq = 21, .inc = 38},
     .description = "Car Idle (VW beetle)"},
    {.effect = {.cont = false, .motor = 1, .fpow = 7, .conv = true, .freq = 57, .inc = 51},
     .description = "Earthquake (Vibrate, and fade out)"},
    {.effect = {.cont = true, .motor = 1, .fpow = 1, .freq = 40, .inc = 5},
     .description = "Helicopter"},
    {.effect = {.cont = false, .motor = 1, .fpow = 2, .freq = 7, .inc = 0},
     .description = "Ship's Thrust (as in AAC)"},
};

/* motor cannot be 0 (will generate error on official hardware), but we can set
 * everything else to 0 for stopping */
static const purupuru_effect_t rumble_stop = {.motor = 1};
static purupuru_effect_t effect = rumble_stop;

static size_t catalog_index = 0;
static int cursor_pos = 0;
static uint16_t old_buttons = 0, rel_buttons = 0;

static const char *fieldnames[] = {
    "cont", "res",  "motor", "bpow", "div", "fpow", "conv", "freq",  "inc"};
static const int num_fields = sizeof(fieldnames) / sizeof(fieldnames[0]);

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

static inline void alterFieldByOffset(int offset, int delta) {
  switch (offset) {
  case 0:
    effect.cont = !effect.cont; // cont
    break;
  case 1:
    break; // res (reserved, cannot be changed)
  case 2:
    effect.motor = (effect.motor + delta) & 0xf; // motor
    if (effect.motor == 0)
      effect.motor = 1; // motor cannot be zero
    break;
  case 3:
    effect.bpow = (effect.bpow + delta) & 0x7; // bpow
    if (effect.bpow)
      effect.fpow = 0; // cannot have both forward and backward power
    break;
  case 4:
    effect.div = !effect.div; // div
    if (effect.conv && effect.div)
      effect.conv = false; // cannot have both convergent and divergent
    break;
  case 5:
    effect.fpow = (effect.fpow + delta) & 0x7; // fpow
    if (effect.fpow)
      effect.bpow = 0; // cannot have both forward and backward power
    break;
  case 6:
    effect.conv = !effect.conv; // conv
    if (effect.conv && effect.div)
      effect.div = false; // cannot have both convergent and divergent
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

void redraw_screen() {
  vid_clear(0, 0, 0);
  int xpos = 128, ypos = 32;
  minifont_set_color(0xff, 0xc0, 0x10); /* gold */
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Rumble Accessory Tester");

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
  xpos += 105;

  char sb[9] = {0};
  sprintf(sb, "0x%lx", effect.raw);
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, sb);

  /* Draw the bottom half of the screen and finish it up. */
  minifont_set_color(255, 255, 255); /* White */
  xpos = 10;
  ypos += 32;

  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, "Field description:");
  minifont_set_color(0, 0, 255); /* Blue */
  minifont_draw_str(vram_s + (640 * ypos) + xpos + 160, 640 , fieldnames[cursor_pos]);
  minifont_draw_str(vram_s + (640 * ypos) + xpos +
                        strlen(fieldnames[cursor_pos]) * 8 + 160,
                    640, ": ");
  ypos += 16;
  minifont_set_color(255, 255, 255); /* White */

  xpos += 20;

  switch (cursor_pos) {
  case 0:
    minifont_draw_str(
        vram_s + (640 * ypos) + xpos, 640,
        "Continuous Vibration. When set vibration will continue until stopped");
    break;
  case 2:
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Motor number. 0 will cause an error. 1 is the typical "
                      "setting. 4-bits.");
    break;
  case 3:
    minifont_draw_str(
        vram_s + (640 * ypos) + xpos, 640,
        "Backward direction (- direction) intensity setting bits.");
    ypos += 16;
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "0 stops vibration. 3-bits. Exclusive with .fpow.");
    break;
  case 4:
    minifont_draw_str(
        vram_s + (640 * ypos) + xpos, 640,
        "Divergent vibration. Make the rumble stronger until it stops.");
    ypos += 16;
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Exclusive with .conv.");
    break;
  case 5:
    minifont_draw_str(
        vram_s + (640 * ypos) + xpos, 640,
        "Forward direction (+ direction) intensity setting bits.");
    ypos += 16;
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "0 stops vibration. 3-bits. Exclusive with .bpow.");
    break;
  case 6:
    minifont_draw_str(
        vram_s + (640 * ypos) + xpos, 640,
        "Convergent vibration. Make the rumble weaker until it stops.");
    ypos += 16;
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Exclusive with .div.");
    break;

  case 7:
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Vibration frequency. For most purupuru the range "
                      "is 4-59. 8-bits.");
    break;
  case 8:
    minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                      "Vibration inclination period setting bits. 8-bits.");
    ypos += 16;
    minifont_draw_str(
        vram_s + (640 * ypos) + xpos, 640,
        "Setting .inc == 0 when .conv or .div are set results in error.");
    break;
  default:
    break;
  }
  ypos = 360;
  xpos = 10;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press left/right to switch field.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press up/down to change values.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press A to send effect to rumblepack.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press B to stop rumble.");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                    "Press X for next baked pattern");
  ypos += 16;
  minifont_draw_str(vram_s + (640 * ypos) + xpos, 640, "Press Start to quit.");

  vid_flip(-1);
}

/* This blocks waiting for a specified device to be present and valid */
void wait_for_dev_attach(maple_device_t **dev_ptr, unsigned int func) {
  maple_device_t *dev = *dev_ptr;
  redraw_screen();
  /* Repeatedly check until we find one and it's valid */
  while ((dev == NULL) || !dev->valid) {
    usleep(10000); // 10ms
    for (int u = 0; u < MAPLE_UNIT_COUNT; u++) {
      *dev_ptr = maple_enum_dev(0, u);
      if (*dev_ptr != NULL && ((*dev_ptr)->info.functions & func)) {
        redraw_screen();
        return;
      }
    }
    dev = *dev_ptr;
    vid_clear(0, 0, 0);
    int xpos = 40, ypos = 200;
    /* Draw up a screen */
    if (func == MAPLE_FUNC_CONTROLLER)
      minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                        "Please attach a controller to port A!");
    else if (func == MAPLE_FUNC_PURUPURU)
      minifont_draw_str(vram_s + (640 * ypos) + xpos, 640,
                        "Please attach a rumbler to controller in port A!");
    vid_flip(-1);
  }
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
    redraw_screen();

    /* Store current button states + buttons which have been released. */
    state = (cont_state_t *)maple_dev_status(contdev);

    /* Make sure we can rely on the state, otherwise loop. */
    if (state == NULL)
      continue;

    rel_buttons = (old_buttons ^ state->buttons);

    if ((state->buttons & CONT_DPAD_LEFT) && (rel_buttons & CONT_DPAD_LEFT)) {
      cursor_pos = cursor_pos - 1;
      if (cursor_pos < 0) {
        cursor_pos = num_fields - 1;
      }
      if (cursor_pos == 1) {
        cursor_pos = 0;
      }
    }

    if ((state->buttons & CONT_DPAD_RIGHT) && (rel_buttons & CONT_DPAD_RIGHT)) {
      cursor_pos = (cursor_pos + 1) % num_fields;
      if (cursor_pos == 1) {
        cursor_pos = (cursor_pos + 1) % num_fields;
      }
    }

    if ((state->buttons & CONT_DPAD_UP)) {
      alterFieldByOffset(cursor_pos, 1);
      usleep(100000); /* 1/10th second */
    }

    if ((state->buttons & CONT_DPAD_DOWN)) {
      alterFieldByOffset(cursor_pos, -1);
      usleep(100000); /* 1/10th second */
    }

    if ((state->buttons & CONT_X) && (rel_buttons & CONT_X)) {
      printf("Setting baked effect:\n\t'%s'\n",
             catalog[catalog_index].description);
      effect = catalog[catalog_index].effect;

      catalog_index++;
      if (catalog_index >= sizeof(catalog) / sizeof(baked_pattern_t)) {
        catalog_index = 0;
      }
    }

    if ((state->buttons & CONT_A) && (rel_buttons & CONT_A)) {
      /* We print these out to make it easier to track the options chosen */
      printf("Rumble: 0x%lx!\n", effect.raw);
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
