#include "bsp_exio.h"
#include "bsp_i2c.h"
#include "esp_check.h"

#define TCA9554_ADDR       0x20
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03  // 0 = output, 1 = input

static const char *TAG = "exio";
static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

esp_err_t bsp_exio_init(void)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev), TAG, "add device");
    ESP_RETURN_ON_ERROR(reg_write(TCA9554_REG_CONFIG, 0x00), TAG, "config (all output)");
    ESP_RETURN_ON_ERROR(reg_write(TCA9554_REG_OUTPUT, 0xFF), TAG, "output high");
    return ESP_OK;
}

esp_err_t bsp_exio_set(uint8_t pin, bool level)
{
    if (pin < 1 || pin > 8) return ESP_ERR_INVALID_ARG;
    uint8_t cur;
    ESP_RETURN_ON_ERROR(reg_read(TCA9554_REG_OUTPUT, &cur), TAG, "read output");
    if (level) cur |= (1u << (pin - 1));
    else       cur &= ~(1u << (pin - 1));
    return reg_write(TCA9554_REG_OUTPUT, cur);
}
