idf_component_register(
    SRCS "bsp_i2c.c" "bsp_exio.c" "st77916_panel.c" "bsp_lvgl.c" "cst816.c"
         "bsp_audio.c" "bsp_sr.c" "bsp_battery.c" "bsp_nvs.c"
         "bsp_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c esp_driver_spi esp_driver_ledc esp_driver_i2s
             esp_lcd esp_lcd_st77916 esp_lcd_touch esp_timer lvgl esp_codec_dev esp-sr esp_adc
             nvs_flash esp_wifi esp_netif esp_event)
