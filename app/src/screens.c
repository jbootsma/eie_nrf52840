#include <lvgl.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "screens.h"

#define NUM_BUTTONS 4
#define LONG_PRESS_MS 1000

typedef struct {
  bool activated;
  screen_fn cb;

  struct {
    bool is_down;
    bool evt_pending;
    int64_t pressed_time;
  } buttons[NUM_BUTTONS];
} screen_info_t;

LOG_MODULE_REGISTER(screens);

static K_MUTEX_DEFINE(button_lock);
static bool button_states[NUM_BUTTONS];
static bool button_changed[NUM_BUTTONS];

static void on_button_evt(struct input_event* evt, void* user_data) {
  if (evt->type != INPUT_EV_KEY) {
    return;
  }

  if (evt->code < INPUT_BTN_0 || evt->code > INPUT_BTN_3) {
    return;
  }

  k_mutex_lock(&button_lock, K_FOREVER);
  size_t idx = evt->code - INPUT_BTN_0;
  button_states[idx] = !!evt->value;
  button_changed[idx] = true;
  k_mutex_unlock(&button_lock);
}
INPUT_CALLBACK_DEFINE(NULL, on_button_evt, NULL);

static void sync_buttons(lv_obj_t* screen, screen_info_t* info) {
  bool lcl_button_changed[NUM_BUTTONS];
  int64_t now = k_uptime_get();

  // Atomically snapshot the state. Ensures consistency while we send events
  // without requiring the lock to be held for a long time.
  k_mutex_lock(&button_lock, K_FOREVER);
  for (size_t i = 0; i < NUM_BUTTONS; i++) {
    // Handle cases where the button changed multiple times between syncs.
    // This will ensure that we act as if something happened, though multiple
    // presses in a short period may still be miss counted.

    lcl_button_changed[i] = button_changed[i] || button_states[i] != info->buttons[i].is_down;
    button_changed[i] = false;
  }
  k_mutex_unlock(&button_lock);

  for (size_t i = 0; i < NUM_BUTTONS; i++) {
    // First apply state changes.
    if (lcl_button_changed[i]) {
      LOG_DBG("Button %zu change", i);

      info->buttons[i].is_down = !info->buttons[i].is_down;
      if (info->buttons[i].is_down) {
        info->buttons[i].evt_pending = true;
        info->buttons[i].pressed_time = now;
      }
    }

    // Now generate events if applicable.
    if (!info->buttons[i].evt_pending || !info->activated) continue;

    screen_evt_t evt = {
      .screen = screen,
      .params.button_code = INPUT_BTN_0 + i,
    };

    if (now - info->buttons[i].pressed_time >= LONG_PRESS_MS) {
      LOG_DBG("Button %zu long press", i);
      evt.code = SCREEN_EVT_BTN_LONG_PRESS;
    }
    else if (!info->buttons[i].is_down) {
      LOG_DBG("Button %zu press", i);
      evt.code = SCREEN_EVT_BTN_PRESS;
    }
    else {
      continue;
    }

    info->buttons[i].evt_pending = false;
    info->cb(evt);
  }
}

static void on_screen_evt(lv_event_t* lv_evt) {
  lv_obj_t* screen = lv_event_get_current_target_obj(lv_evt);
  screen_info_t* info = lv_obj_get_user_data(screen);
  screen_evt_t evt = {
    .screen = screen,
  };

  switch (lv_event_get_code(lv_evt)) {
  case LV_EVENT_SCREEN_LOAD_START:
    evt.code = SCREEN_EVT_INIT;
    break;

  case LV_EVENT_SCREEN_LOADED:
    evt.code = SCREEN_EVT_ACTIVATE;
    info->activated = true;
    break;

  case LV_EVENT_SCREEN_UNLOAD_START:
    evt.code = SCREEN_EVT_DEACTIVATE;
    info->activated = false;
    break;

  case LV_EVENT_SCREEN_UNLOADED:
    evt.code = SCREEN_EVT_DESTROY;
    break;

  default:
    break;
  }

  if (evt.code != SCREEN_EVT_NONE) {
    LOG_DBG("Screen evt %d for %p", evt.code, screen);
    info->cb(evt);
  }

  if (evt.code == SCREEN_EVT_DESTROY) {
    lv_free(info);
  }
}

void init_screen_mgr(screen_fn init_cb) {
  change_to_screen(init_cb, LV_SCR_LOAD_ANIM_NONE, 0);
}

void update_screen_mgr(void) {
  lv_obj_t* cur = lv_screen_active();
  screen_info_t* info = lv_obj_get_user_data(cur);

  if (info && info->activated) {
    sync_buttons(cur, info);

    screen_evt_t evt = {
      .code = SCREEN_EVT_UPDATE,
      .screen = cur,
    };
    info->cb(evt);
  }

  lv_timer_handler();
}

void change_to_screen(screen_fn cb, lv_screen_load_anim_t anim, uint32_t anim_time) {
  lv_obj_t* screen = lv_obj_create(NULL);

  LOG_INF("Change to screen %p", screen);

  // Use the object user data rather than the event handler, as we need to
  // access outside the context of an event as well (eg. during the core update loop).
  screen_info_t* info = lv_malloc_zeroed(sizeof(*info));
  info->cb = cb;
  lv_obj_set_user_data(screen, info);

  lv_obj_add_event_cb(screen, on_screen_evt, LV_EVENT_ALL, NULL);
  lv_scr_load_anim(screen, anim, anim_time, 0, true);
}
