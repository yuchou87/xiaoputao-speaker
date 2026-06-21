#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

// Shared I2C0 master bus: SCL=GPIO10, SDA=GPIO11.
// Used by TCA9554, CST816 touch, ES8311, ES7210, PCF85063.
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_bus(void);
