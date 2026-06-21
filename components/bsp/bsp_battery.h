#pragma once
#include "esp_err.h"

// Battery voltage via ADC1_CH7, 3x divider (formula from board demo).
esp_err_t bsp_battery_init(void);
float bsp_battery_voltage(void);   // volts; ~0 / floating when on USB without a cell
