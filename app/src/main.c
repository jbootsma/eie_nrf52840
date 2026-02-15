/**
 * @file main.c
 */

#include <inttypes.h>

#include <lvgl.h>
#include <lvgl_mem.h>
#include <zephyr/debug/cpu_load.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "screens.h"

#define SLEEP_MS 1

#define DBG_HEAP_STATS 0

static const struct device* const disp = DEVICE_DT_GET(DISP_NODE);

LOG_MODULE_REGISTER(main);

int main(void) {
  LOG_INF("Initializing");

  if (!device_is_ready(disp)) {
    LOG_ERR("Display was not ready");
    return -1;
  }

  init_screen_mgr(color_dbg_screen);

  if (0 != display_blanking_off(disp)) {
    LOG_ERR("Could not enable display");
  }

  LOG_INF("Init complete");

  size_t frame = 0;
  int64_t start_time = k_uptime_get();

  while (1) {
    update_screen_mgr();
    k_msleep(SLEEP_MS);

    frame += 1;
    int64_t now = k_uptime_get();
    if ((now - start_time) > 3000) {
      LOG_INF("Avg FPS: %0.1f", (double)(frame * 1000) / (now - start_time));
#if CONFIG_CPU_LOAD
      LOG_INF("Load: %.01f", cpu_load_get(true) / 10.0);
#endif

#if CONFIG_SYS_HEAP_RUNTIME_STATS
      // The LVGL integration doesn't fully fill out mem info when queried through lv_mem_monitor :(
      // gotta use zephyr-specific API instead.
      struct sys_memory_stats memstats;
      lvgl_heap_stats(&memstats);
      size_t total = memstats.allocated_bytes + memstats.free_bytes;
      size_t pct = (memstats.allocated_bytes * 100) / total;
      size_t max_pct = (memstats.max_allocated_bytes * 100) / total;

      LOG_INF("LVGL Heap: %zu/%zu used (%zu%%). Max %zu (%zu%%)",
        memstats.allocated_bytes, total, pct, memstats.max_allocated_bytes, max_pct);

#if DBG_HEAP_STATS
      lvgl_print_heap_info(false);
#endif
#endif

      start_time = now;
      frame = 0;
    }
  }
  return 0;
}
