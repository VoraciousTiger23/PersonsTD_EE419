#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "WiFi_Connect.h"

#define RGB_RED_GPIO   GPIO_NUM_5
#define RGB_GREEN_GPIO GPIO_NUM_6
#define RGB_BLUE_GPIO  GPIO_NUM_9

static const char *TAG = "EE419_LAB1";

static void rgb_set_color(bool red, bool green, bool blue)
{
    gpio_set_level(RGB_RED_GPIO, red ? 1 : 0);
    gpio_set_level(RGB_GREEN_GPIO, green ? 1 : 0);
    gpio_set_level(RGB_BLUE_GPIO, blue ? 1 : 0);
}

static void led_color_cycle_task(void *arg)
{
    const struct {
        bool red;
        bool green;
        bool blue;
    } colors[] = {
        {true,  true,  true }, // White
        {true,  false, false}, // Red
        {false, true,  false}, // Green
        {false, false, true }, // Blue
        {true,  true,  false}, // Yellow
        {true,  false, true }, // Magenta
        {false, true,  true }, // Cyan
        {false, false, false}  // Off
    };

    while (1) {
        for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
            rgb_set_color(colors[i].red, colors[i].green, colors[i].blue);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RGB_RED_GPIO) |
                        (1ULL << RGB_GREEN_GPIO) |
                        (1ULL << RGB_BLUE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    xTaskCreate(&led_color_cycle_task, "led_color_cycle", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Starting EE419 lab application");
    WiFi_Connect();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
