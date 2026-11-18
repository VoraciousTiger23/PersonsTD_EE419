idf_component_register(SRCS "main.c" "WiFi_Connect.c" "clrGen.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_http_server
                    PRIV_REQUIRES esp_driver_gpio esp_driver_spi)
