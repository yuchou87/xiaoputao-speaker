#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"

static const char *TAG = "xpt";

void app_main(void)
{
    ESP_LOGI(TAG, "xiaoputao-speaker P0 bring-up boot");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s, %d core(s), rev %d",
             CONFIG_IDF_TARGET, chip.cores, chip.revision);

    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t intram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "PSRAM free:    %u bytes (%.1f MB)", (unsigned) psram, psram / 1048576.0);
    ESP_LOGI(TAG, "Internal free: %u bytes (%.0f KB)", (unsigned) intram, intram / 1024.0);

    if (psram < 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "PSRAM too small or not detected! expected ~8MB");
    } else {
        ESP_LOGI(TAG, "PSRAM OK");
    }
}
