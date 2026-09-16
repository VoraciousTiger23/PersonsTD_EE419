#include "RFID_Sensor.h"

#include <stddef.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pn532.h"
#include "pn532_driver_spi.h"
#include "driver/spi_master.h"

static const char *TAG = "RFID_Sensor";

#define RGB_RED_GPIO   GPIO_NUM_5
#define RGB_GREEN_GPIO GPIO_NUM_6
#define RGB_BLUE_GPIO  GPIO_NUM_9

// PN532 SPI pins (from user wiring)
#define PN532_SCK_PIN  ((gpio_num_t)10)
#define PN532_MISO_PIN ((gpio_num_t)11)
#define PN532_MOSI_PIN ((gpio_num_t)12)
#define PN532_SS_PIN   ((gpio_num_t)13)

// NVS namespace/key
#define NVS_NAMESPACE "rfid"
#define NVS_KEY_SAVED_UID "saved_uid"

void rgb_set_color(bool red, bool green, bool blue)
{
    gpio_set_level(RGB_RED_GPIO, red ? 1 : 0);
    gpio_set_level(RGB_GREEN_GPIO, green ? 1 : 0);
    gpio_set_level(RGB_BLUE_GPIO, blue ? 1 : 0);
}

static bool uid_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len)
{
    if (a_len != b_len) return false;
    return memcmp(a, b, a_len) == 0;
}

// Task: initialize PN532 and continuously scan for tags
static void pn532_scan_task(void *arg)
{
    // Initialize NVS handle
    nvs_handle_t nvs_handle;
    bool has_saved = false;
    uint8_t *saved_uid = NULL;
    size_t saved_len = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        // read blob size
        esp_err_t err = nvs_get_blob(nvs_handle, NVS_KEY_SAVED_UID, NULL, &saved_len);
        if (err == ESP_OK && saved_len > 0) {
            saved_uid = calloc(1, saved_len);
            if (saved_uid) {
                if (nvs_get_blob(nvs_handle, NVS_KEY_SAVED_UID, saved_uid, &saved_len) == ESP_OK) {
                    has_saved = true;
                    ESP_LOGI(TAG, "Loaded saved UID (len=%d)", (int)saved_len);
                } else {
                    free(saved_uid);
                    saved_uid = NULL;
                    saved_len = 0;
                }
            }
        }
    }

    // Configure LED GPIOs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RGB_RED_GPIO) | (1ULL << RGB_GREEN_GPIO) | (1ULL << RGB_BLUE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // If no saved UID, show blue until first detection
    if (!has_saved) {
        rgb_set_color(false, false, true);
    } else {
        // start with LED off
        rgb_set_color(false, false, false);
    }

    // allocate io handle
    pn532_io_t io;
    pn532_io_handle_t io_handle = &io;

    // initialize driver struct
    memset(io_handle, 0, sizeof(io));

    esp_err_t err = pn532_new_driver_spi(PN532_MISO_PIN, PN532_MOSI_PIN, PN532_SCK_PIN,
                                         PN532_SS_PIN, GPIO_NUM_NC, GPIO_NUM_NC,
                                         SPI2_HOST, 4000000, io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532_new_driver_spi failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    if ((err = pn532_init(io_handle)) != ESP_OK) {
        ESP_LOGE(TAG, "pn532_init failed: %d", err);
        pn532_delete_driver(io_handle);
        vTaskDelete(NULL);
        return;
    }

    // configure for normal mode
    pn532_SAM_config(io_handle);

    uint8_t uid[10];
    uint8_t uid_len = sizeof(uid);

    while (1) {
        uid_len = sizeof(uid);
        esp_err_t r = pn532_read_passive_target_id(io_handle, PN532_BRTY_ISO14443A_106KBPS, uid, &uid_len, 1000);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "Tag detected len=%d", uid_len);
            if (!has_saved) {
                // store first seen UID to NVS
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
                    esp_err_t serr = nvs_set_blob(nvs_handle, NVS_KEY_SAVED_UID, uid, uid_len);
                    if (serr == ESP_OK) {
                        nvs_commit(nvs_handle);
                        has_saved = true;
                        saved_len = uid_len;
                        saved_uid = malloc(saved_len);
                        if (saved_uid) memcpy(saved_uid, uid, saved_len);
                        ESP_LOGI(TAG, "Saved UID to NVS");
                    } else {
                        ESP_LOGE(TAG, "Failed to save UID to NVS: %d", serr);
                    }
                    nvs_close(nvs_handle);
                }
                // show green for saved tag
                rgb_set_color(false, true, false);
            } else {
                // compare with saved
                if (uid_equal(uid, uid_len, saved_uid, saved_len)) {
                    rgb_set_color(false, true, false); // green
                } else {
                    rgb_set_color(true, false, false); // red
                }
            }
        } else {
            // no tag detected in timeout
            if (!has_saved) {
                // keep blue until first detection
                rgb_set_color(false, false, true);
            } else {
                rgb_set_color(false, false, false); // off
            }
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // cleanup (never reached)
    if (saved_uid) free(saved_uid);
    pn532_delete_driver(io_handle);
    vTaskDelete(NULL);
}

void RFID_Sensor_init(void)
{
    // start PN532 scanning task
    BaseType_t r = xTaskCreate(&pn532_scan_task, "pn532_scan", 4096, NULL, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pn532_scan task");
    }
}
