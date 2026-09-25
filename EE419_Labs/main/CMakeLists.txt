idf_component_register(SRCS "RFID_Webpage.c" "main.c" "WiFi_Connect.c" "RFID_Sensor.c" "MQTT_RPi.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_http_server nvs_flash pn532 mdns lwip mqtt
                    PRIV_REQUIRES esp_driver_gpio)
