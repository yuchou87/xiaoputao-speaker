idf_component_register(
    SRCS "bsp_i2c.c" "bsp_exio.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c)
