idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_http_server nvs_flash
                    PRIV_REQUIRES esp_driver_gpio)
