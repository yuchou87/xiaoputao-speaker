#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// TCA9554 I2C IO expander (addr 0x20). Pins are 1..8.
// On this board: EXIO1 = touch reset, EXIO2 = LCD reset.
#define EXIO_TOUCH_RST 1
#define EXIO_LCD_RST   2

esp_err_t bsp_exio_init(void);                  // all pins -> output, default high
esp_err_t bsp_exio_set(uint8_t pin, bool level); // pin 1..8
