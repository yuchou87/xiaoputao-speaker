#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "bsp_i2c.h"
#include "bsp_exio.h"

static const char *TAG = "xpt";

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scan:");
    int found = 0;
    for (uint8_t a = 1; a < 0x7f; a++) {
        if (i2c_master_probe(bsp_i2c_bus(), a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  found 0x%02X", a);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan done, %d device(s)", found);
}

void app_main(void)
{
    ESP_LOGI(TAG, "xiaoputao-speaker P0 bring-up boot");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s, %d core(s), rev %d", CONFIG_IDF_TARGET, chip.cores, chip.revision);

    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM free: %u bytes (%.1f MB)", (unsigned) psram, psram / 1048576.0);

    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_scan();

    ESP_ERROR_CHECK(bsp_exio_init());
    ESP_LOGI(TAG, "TCA9554 init ok");
}
