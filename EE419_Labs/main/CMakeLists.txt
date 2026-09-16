idf_component_register(SRCS "main.c" "WiFi_Connect.c" "RFID_Sensor.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_http_server nvs_flash pn532
                    PRIV_REQUIRES esp_driver_gpio)
