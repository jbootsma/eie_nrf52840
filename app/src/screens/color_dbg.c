#include "screens.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(main);

static const lv_style_const_prop_t block_props[] = {
  LV_STYLE_CONST_WIDTH(WIDTH / 3),
  LV_STYLE_CONST_HEIGHT(HEIGHT / 2),
  LV_STYLE_CONST_RADIUS(0),
  LV_STYLE_CONST_BORDER_WIDTH(0),
  LV_STYLE_CONST_PROPS_END,
};

static LV_STYLE_CONST_INIT(block_style, block_props);

static const lv_color_t colors[] = {
  LV_COLOR_MAKE(255, 0, 0),
  LV_COLOR_MAKE(0, 255, 0),
  LV_COLOR_MAKE(0, 0, 255),

  LV_COLOR_MAKE(0, 255, 255),
  LV_COLOR_MAKE(255, 0, 255),
  LV_COLOR_MAKE(255, 255, 0),
};

void color_dbg_screen(screen_evt_t evt) {
  static int64_t active_time;

  switch (evt.code) {
  case SCREEN_EVT_INIT:
    for (size_t y = 0; y < 2; y++) {
      for (size_t x = 0; x < 3; x++) {
        lv_obj_t* block = lv_obj_create(evt.screen);
        lv_obj_add_style(block, &block_style, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(block, colors[y * 3 + x], LV_STATE_DEFAULT);
        lv_obj_set_x(block, WIDTH / 3 * x);
        lv_obj_set_y(block, HEIGHT / 2 * y);
      }
    }
    break;

  case SCREEN_EVT_ACTIVATE:
    active_time = k_uptime_get();
    break;

  case SCREEN_EVT_BTN_PRESS:
  case SCREEN_EVT_BTN_LONG_PRESS:
    change_to_screen(touch_dbg_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200);
    break;

  default:
    break;
  }
}
