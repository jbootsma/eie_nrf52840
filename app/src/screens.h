#ifndef DEBUG_SCREENS_H
#define DEBUG_SCREENS_H

#include <lvgl.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>

#define DISP_NODE DT_CHOSEN(zephyr_display)
#define WIDTH DT_PROP(DISP_NODE, width)
#define HEIGHT DT_PROP(DISP_NODE, height)

typedef enum {
  SCREEN_EVT_NONE,

  // Lifetime events

  /// Prepare initial screen items, screen transition is about to be animated.
  SCREEN_EVT_INIT,
  /// Screen transition is complete, input and update events will now be enabled.
  SCREEN_EVT_ACTIVATE,
  /// Once per frame chance to run logic.
  SCREEN_EVT_UPDATE,
  /// Screen is about to transition, no more input/update events will be received
  SCREEN_EVT_DEACTIVATE,
  /// Screen is no longer visible. Release all resources
  /// (the widget tree will be freed by the screen manager).
  SCREEN_EVT_DESTROY,

  // Button events, button_code indicates the relevant button.
  // Simulated button events will be generated as appropriate during screen transitions.
  SCREEN_EVT_BTN_PRESS,
  SCREEN_EVT_BTN_LONG_PRESS,
} screen_evt_e;

typedef struct {
  /// Top of the widget tree.
  /// @note The user data of the screen node is reserved for use by the screen manager.
  lv_obj_t* screen;

  /// Event that occurred
  screen_evt_e code;

  /// Parameters associated with the event.
  union {
    uint16_t button_code;
  } params;
} screen_evt_t;

typedef void (*screen_fn)(screen_evt_t evt);

void init_screen_mgr(screen_fn init_cb);
void update_screen_mgr(void);
void change_to_screen(screen_fn cb, lv_screen_load_anim_t anim, uint32_t anim_time);

// Screen implementations
void color_dbg_screen(screen_evt_t evt);
void touch_dbg_screen(screen_evt_t evt);

#endif
