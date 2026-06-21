#pragma once
#include "esp_err.h"

// Initialise LVGL bound to the ST77916 panel (call after LCD_Init()).
// Starts a 2ms tick timer and a background lv_timer_handler task.
esp_err_t bsp_lvgl_init(void);

// Guard every LVGL API call from outside the LVGL task with these.
void bsp_lvgl_lock(void);
void bsp_lvgl_unlock(void);
