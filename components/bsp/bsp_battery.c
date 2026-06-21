#include "bsp_battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_check.h"

#define BAT_CHAN        ADC_CHANNEL_7   // GPIO8 on ESP32-S3
#define BAT_ATTEN       ADC_ATTEN_DB_12
#define BAT_DIVIDER     3.0f            // hardware divider
#define BAT_OFFSET      0.9945f         // demo's correction factor

static const char *TAG = "bsp_bat";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

esp_err_t bsp_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc), TAG, "adc unit");

    adc_oneshot_chan_cfg_t ccfg = { .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BAT_CHAN, &ccfg), TAG, "adc chan");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = BAT_CHAN,
        .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);
#endif
    ESP_LOGI(TAG, "battery ADC ready (cali=%d)", s_cali_ok);
    return ESP_OK;
}

float bsp_battery_voltage(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_CHAN, &raw) != ESP_OK) return 0.0f;
    int mv = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return (mv * BAT_DIVIDER / 1000.0f) / BAT_OFFSET;
    }
    return 0.0f;
}
