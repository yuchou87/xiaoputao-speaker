idf_component_register(
    SRCS "bsp_i2c.c" "bsp_exio.c" "st77916_panel.c" "bsp_lvgl.c" "cst816.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c esp_driver_spi esp_driver_ledc esp_lcd esp_lcd_st77916 esp_lcd_touch esp_timer lvgl)
