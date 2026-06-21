#include "bsp_nvs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE "xpt"

static const char *TAG = "bsp_nvs";

esp_err_t bsp_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or version mismatch, erasing...");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bsp_nvs_get_str(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h), TAG, "nvs_open");
    esp_err_t ret = nvs_get_str(h, key, out, &out_len);
    nvs_close(h);
    return ret;
}

esp_err_t bsp_nvs_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t ret = nvs_set_str(h, key, val);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "nvs_set_str failed: %s", esp_err_to_name(ret));
    }
    nvs_close(h);
    return ret;
}

bool bsp_nvs_has(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t required_size = 0;
    esp_err_t ret = nvs_get_str(h, key, NULL, &required_size);
    nvs_close(h);
    return ret == ESP_OK;
}
