#include "screens.h"

#include <lvgl_input_device.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(touch_dbg, LOG_LEVEL_WRN);

static const struct device* const pointer = DEVICE_DT_GET(DT_INST(0, zephyr_lvgl_pointer_input));

static volatile bool touch_active = false;
static lv_obj_t* marker;

static const lv_style_const_prop_t marker_props[] = {
  LV_STYLE_CONST_BG_COLOR(LV_COLOR_MAKE(0, 0, 0)),
  LV_STYLE_CONST_BORDER_WIDTH(0),
  LV_STYLE_CONST_RADIUS(0),
  LV_STYLE_CONST_WIDTH(9),
  LV_STYLE_CONST_HEIGHT(9),
  LV_STYLE_CONST_TRANSLATE_X(-5),
  LV_STYLE_CONST_TRANSLATE_Y(-5),
  LV_STYLE_CONST_PROPS_END,
};

static LV_STYLE_CONST_INIT(marker_style, marker_props);

static void touch_monitor(lv_event_t* evt) {
  switch (lv_event_get_code(evt)) {
  case LV_EVENT_PRESSED:
    LOG_INF("Touch active");
    touch_active = true;
    break;

  case LV_EVENT_RELEASED:
    LOG_INF("Touch no longer active");
    touch_active = false;
    break;

  default:
    break;
  }
}

void touch_dbg_screen(screen_evt_t evt) {
  switch (evt.code) {
  case SCREEN_EVT_ACTIVATE: {
    lv_obj_set_flag(evt.screen, LV_OBJ_FLAG_SCROLLABLE, false);
    marker = lv_obj_create(evt.screen);
    lv_obj_add_style(marker, &marker_style, LV_STATE_DEFAULT);
    lv_obj_set_flag(marker, LV_OBJ_FLAG_HIDDEN, true);
    lv_indev_add_event_cb(lvgl_input_get_indev(pointer), touch_monitor, LV_EVENT_ALL, NULL);
  } break;

  case SCREEN_EVT_UPDATE: {
    lv_obj_set_flag(marker, LV_OBJ_FLAG_HIDDEN, !touch_active);
    if (touch_active) {
      lv_point_t point;
      lv_indev_get_point(lvgl_input_get_indev(pointer), &point);
      LOG_DBG("x=%d y=%d", point.x, point.y);
      lv_obj_set_x(marker, point.x);
      lv_obj_set_y(marker, point.y);
    }
  } break;

  case SCREEN_EVT_DEACTIVATE: {
    lv_indev_remove_event_cb_with_user_data(lvgl_input_get_indev(pointer), touch_monitor, NULL);
    touch_active = false;
  } break;

  case SCREEN_EVT_BTN_PRESS:
  case SCREEN_EVT_BTN_LONG_PRESS:
    change_to_screen(color_dbg_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200);
    break;

  default:
    break;
  }
}
