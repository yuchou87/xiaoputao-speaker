#include "bsp_i2c.h"

#define BSP_I2C_SCL 10
#define BSP_I2C_SDA 11

static i2c_master_bus_handle_t s_bus;

esp_err_t bsp_i2c_init(void)
{
    if (s_bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_bus; }
