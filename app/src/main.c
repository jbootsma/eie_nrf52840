/**
 * @file main.c
 */

#include <inttypes.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#if CONFIG_GPIO
#include "BTN.h"
#include "LED.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define DISP_NODE DT_CHOSEN(zephyr_display)
#define WIDTH DT_PROP(DISP_NODE, width)
#define HEIGHT DT_PROP(DISP_NODE, height)

static const struct device* disp = DEVICE_DT_GET(DISP_NODE);

// Use RGB565 on hardware for speed, but RGB888 on SDL. There's bugs on our version of zephyr
// around byte ordering in 565 mode.

#if CONFIG_BOARD_NATIVE_SIM

#define PIXFMT PIXEL_FORMAT_RGB_888

typedef struct {
  uint8_t b;
  uint8_t g;
  uint8_t r;
} pixel_t;

#define RGB(r_, g_, b_) (pixel_t){.r = (r_), .g = (g_), .b = (b_)}

#else

#define PIXFMT PIXEL_FORMAT_RGB_565

typedef struct {
  uint16_t b : 5;
  uint16_t g : 6;
  uint16_t r : 5;
} pixel_t;

#define RGB(r_, g_, b_) (pixel_t){.r = (r_) >> 3, .g = (g_) >> 2, .b = (b_) >> 3}

#endif

#define HEX_COL(v) RGB(((v) >> 16) & 0xff, ((v) >> 8) & 0xff, (v) & 0xff)

typedef pixel_t frame_t[HEIGHT][WIDTH];

// There's not enough memory to have two frames in the frame buffer.
// So instead achieve double-bufferring by splitting the frame into a top and
// bottom half. One half can be updated while the other is DMAd to the display.

static frame_t frame;
// Indicates half the frame is ready and can be sent to the display.
static K_SEM_DEFINE(frame_rendered_sem, 0, 2);
// Indicates that the frame has been drawn and can now be rendered to again.
static K_SEM_DEFINE(frame_available_sem, 2, 2);

#define FADE_IN(msk, base) \
  HEX_COL((0x000000 & (msk)) | (base)), \
  HEX_COL((0x333333 & (msk)) | (base)), \
  HEX_COL((0x666666 & (msk)) | (base)), \
  HEX_COL((0x888888 & (msk)) | (base)), \
  HEX_COL((0xaaaaaa & (msk)) | (base)), \
  HEX_COL((0xbbbbbb & (msk)) | (base)), \
  HEX_COL((0xcccccc & (msk)) | (base)), \
  HEX_COL((0xdddddd & (msk)) | (base)), \
  HEX_COL((0xeeeeee & (msk)) | (base)), \
  HEX_COL((0xffffff & (msk)) | (base))

#define FADE_OUT(msk, base) \
  HEX_COL((0xffffff & (msk)) | (base)), \
  HEX_COL((0xeeeeee & (msk)) | (base)), \
  HEX_COL((0xdddddd & (msk)) | (base)), \
  HEX_COL((0xcccccc & (msk)) | (base)), \
  HEX_COL((0xbbbbbb & (msk)) | (base)), \
  HEX_COL((0xaaaaaa & (msk)) | (base)), \
  HEX_COL((0x888888 & (msk)) | (base)), \
  HEX_COL((0x666666 & (msk)) | (base)), \
  HEX_COL((0x333333 & (msk)) | (base)), \
  HEX_COL((0x000000 & (msk)) | (base))

#define PULSE(msk, base) \
  FADE_IN(msk, base), \
  FADE_OUT(msk, base)

static const pixel_t cols[] = {
  PULSE(0xFF0000, 0x000000),
  PULSE(0x00FF00, 0x000000),
  FADE_IN(0x0000FF, 0x000000),
  FADE_IN(0x00FF00, 0x0000FF),
  FADE_OUT(0x0000FF, 0x00FF00),
  FADE_IN(0xFF0000, 0x00FF00),
  FADE_OUT(0x00FF00, 0xFF0000),
  FADE_IN(0x00FFFF, 0xFF0000),
  FADE_OUT(0xFFFFFF, 0x000000),
};

#define CHECK_CALL(exp) do {\
  int res = (exp);\
  if (0 != res) {\
    LOG_ERR("%s failed with err %d", #exp, res); \
    return res;\
  }\
} while (0)

int main(void) {

#if CONFIG_GPIO
  if (0 > BTN_init()) {
    return 0;
  }
  if (0 > LED_init()) {
    return 0;
  }
#endif

  if (!device_is_ready(disp)) {
    LOG_ERR("Display init failed");
    return 0;
  }

  LOG_INF("Setting up display");
  CHECK_CALL(display_blanking_on(disp));
  CHECK_CALL(display_set_pixel_format(disp, PIXFMT));
  // Orientation not supported on this version of sim.
#if !CONFIG_BOARD_NATIVE_SIM
  CHECK_CALL(display_set_orientation(disp, DISPLAY_ORIENTATION_NORMAL));
#endif
  CHECK_CALL(display_clear(disp));
  CHECK_CALL(display_blanking_off(disp));

  while (true) {
    for (size_t i = 0; i < ARRAY_SIZE(cols); i++) {
      k_sem_take(&frame_available_sem, K_FOREVER);

      for (size_t y = 0; y < HEIGHT / 2; y++) {
        pixel_t pix = cols[(i + y / 5) % ARRAY_SIZE(cols)];

        for (size_t x = 0; x < WIDTH; x++) {
          frame[y][x] = pix;
        }
      }

      k_sem_give(&frame_rendered_sem);

      k_sem_take(&frame_available_sem, K_FOREVER);

      for (size_t y = HEIGHT / 2; y < HEIGHT; y++) {
        pixel_t pix = cols[(i + y / 5) % ARRAY_SIZE(cols)];

        for (size_t x = 0; x < WIDTH; x++) {
          frame[y][x] = pix;
        }
      }

      k_sem_give(&frame_rendered_sem);
    }
  }

  return 0;
}

void disp_loop(void*, void*, void*) {
  int64_t start_time = k_uptime_get();
  int frame_counter = 0;

  struct display_buffer_descriptor desc = {
        .buf_size = sizeof(*frame),
        .width = WIDTH,
        .height = HEIGHT / 2,
        .pitch = WIDTH,
        .frame_incomplete = true,
  };

  while (true) {
    k_sem_take(&frame_rendered_sem, K_FOREVER);

    desc.frame_incomplete = true;
    int ret = display_write(disp, 0, 0, &desc, frame);

    if (ret != 0) {
      LOG_ERR("Failed to write frame: %d", ret);
      break;
    }

    k_sem_give(&frame_available_sem);

    k_sem_take(&frame_rendered_sem, K_FOREVER);

    desc.frame_incomplete = false;
    ret = display_write(disp, 0, HEIGHT / 2, &desc, frame[HEIGHT / 2]);

    if (ret != 0) {
      LOG_ERR("Failed to write frame: %d", ret);
      break;
    }

#if CONFIG_BOARD_NATIVE_SIM
    // Need to let time actually advance. Otherwise we get infinite fps.
    k_sleep(K_MSEC(1));
#endif

    k_sem_give(&frame_available_sem);

    if (++frame_counter == 10) {
      int64_t now = k_uptime_get();
      int64_t delta = now - start_time;

      // Cancel out 10 frames per measurement, and 1000 ms per second.
      LOG_INF("FPS: %.02f", 10000.0 / delta);

      frame_counter = 0;
      start_time = now;
    }
  }
}

K_THREAD_DEFINE(disp_thread, CONFIG_MAIN_STACK_SIZE, disp_loop, NULL, NULL, NULL, K_PRIO_PREEMPT(0), K_FP_REGS, 0);
