idf_component_register(SRCS "glm_realtime.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_websocket_client json bsp esp_event esp-tls mbedtls)
