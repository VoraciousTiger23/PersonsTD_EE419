#include "RFID_Sensor.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "pn532.h"
#include "pn532_driver_spi.h"
#include "driver/spi_master.h"

#include "MQTT_RPi.h"

static const char *TAG = "RFID_Sensor";

#define RGB_RED_GPIO   GPIO_NUM_5
#define RGB_GREEN_GPIO GPIO_NUM_6
#define RGB_BLUE_GPIO  GPIO_NUM_9

// PN532 SPI pins (from user wiring)
#define PN532_SCK_PIN  ((gpio_num_t)10)
#define PN532_MISO_PIN ((gpio_num_t)11)
#define PN532_MOSI_PIN ((gpio_num_t)12)
#define PN532_SS_PIN   ((gpio_num_t)13)

// Last-detected UID storage (shared with web UI)
static uint8_t s_last_uid[10];
static size_t s_last_uid_len = 0;
static bool s_last_uid_present = false;
static SemaphoreHandle_t s_last_uid_mutex = NULL;

// Saved (target) UID storage, supplied by MQTT at runtime and kept in memory only.
static uint8_t s_saved_uid[10];
static size_t s_saved_uid_len = 0;
static bool s_saved_uid_present = false;
static SemaphoreHandle_t s_saved_uid_mutex = NULL;

static int s_flash_count = 0;
static SemaphoreHandle_t s_flash_count_mutex = NULL;

void RFID_set_last_uid(const uint8_t *uid, size_t len)
{
    if (!s_last_uid_mutex) s_last_uid_mutex = xSemaphoreCreateMutex();
    if (!s_last_uid_mutex) return;
    if (xSemaphoreTake(s_last_uid_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t n = len;
        if (n > sizeof(s_last_uid)) n = sizeof(s_last_uid);
        memcpy(s_last_uid, uid, n);
        s_last_uid_len = n;
        s_last_uid_present = true;
        xSemaphoreGive(s_last_uid_mutex);
    }
}

bool RFID_get_last_uid(uint8_t *buf, size_t *len)
{
    if (!s_last_uid_mutex) s_last_uid_mutex = xSemaphoreCreateMutex();
    if (!s_last_uid_mutex) return false;
    bool present = false;
    if (xSemaphoreTake(s_last_uid_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_last_uid_present) {
            memcpy(buf, s_last_uid, s_last_uid_len);
            *len = s_last_uid_len;
            present = true;
        } else {
            *len = 0;
            present = false;
        }
        xSemaphoreGive(s_last_uid_mutex);
    }
    return present;
}

void RFID_clear_last_uid(void)
{
    if (!s_last_uid_mutex) s_last_uid_mutex = xSemaphoreCreateMutex();
    if (!s_last_uid_mutex) return;
    if (xSemaphoreTake(s_last_uid_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_last_uid_present = false;
        s_last_uid_len = 0;
        xSemaphoreGive(s_last_uid_mutex);
    }
}

void RFID_set_saved_uid(const uint8_t *uid, size_t len)
{
    if (!s_saved_uid_mutex) s_saved_uid_mutex = xSemaphoreCreateMutex();
    if (!s_saved_uid_mutex) return;
    if (xSemaphoreTake(s_saved_uid_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t n = len;
        if (n > sizeof(s_saved_uid)) n = sizeof(s_saved_uid);
        memcpy(s_saved_uid, uid, n);
        s_saved_uid_len = n;
        s_saved_uid_present = true;
        xSemaphoreGive(s_saved_uid_mutex);
    }
}

bool RFID_get_saved_uid(uint8_t *buf, size_t *len)
{
    if (!s_saved_uid_mutex) s_saved_uid_mutex = xSemaphoreCreateMutex();
    if (!s_saved_uid_mutex) return false;
    bool present = false;
    if (xSemaphoreTake(s_saved_uid_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_saved_uid_present) {
            memcpy(buf, s_saved_uid, s_saved_uid_len);
            *len = s_saved_uid_len;
            present = true;
        } else {
            *len = 0;
            present = false;
        }
        xSemaphoreGive(s_saved_uid_mutex);
    }
    return present;
}

void RFID_clear_saved_uid(void)
{
    if (!s_saved_uid_mutex) s_saved_uid_mutex = xSemaphoreCreateMutex();
    if (!s_saved_uid_mutex) return;
    if (xSemaphoreTake(s_saved_uid_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_saved_uid_present = false;
        s_saved_uid_len = 0;
        xSemaphoreGive(s_saved_uid_mutex);
    }
}

void RFID_set_flash_count(int count)
{
    if (!s_flash_count_mutex) s_flash_count_mutex = xSemaphoreCreateMutex();
    if (!s_flash_count_mutex) return;
    if (xSemaphoreTake(s_flash_count_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_flash_count = count < 0 ? 0 : count;
        xSemaphoreGive(s_flash_count_mutex);
    }
}

int RFID_get_flash_count(void)
{
    if (!s_flash_count_mutex) s_flash_count_mutex = xSemaphoreCreateMutex();
    if (!s_flash_count_mutex) return 0;
    int count = 0;
    if (xSemaphoreTake(s_flash_count_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = s_flash_count;
        xSemaphoreGive(s_flash_count_mutex);
    }
    return count;
}

void RFID_set_target_tag_hex(const char *tag_hex)
{
    if (!tag_hex) {
        RFID_clear_saved_uid();
        return;
    }

    char cleaned[17] = {0};
    size_t cleaned_len = 0;
    for (const char *p = tag_hex; *p != '\0' && cleaned_len < sizeof(cleaned) - 1; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isxdigit(c)) {
            cleaned[cleaned_len++] = (char)toupper(c);
        }
    }

    if (cleaned_len == 0) {
        RFID_clear_saved_uid();
        return;
    }

    if (cleaned_len % 2 != 0) {
        cleaned_len--;
    }

    uint8_t raw[10] = {0};
    size_t raw_len = cleaned_len / 2;
    if (raw_len > sizeof(raw)) {
        raw_len = sizeof(raw);
    }

    for (size_t i = 0; i < raw_len; ++i) {
        char pair[3] = { cleaned[i * 2], cleaned[i * 2 + 1], '\0' };
        raw[i] = (uint8_t)strtoul(pair, NULL, 16);
    }

    RFID_set_saved_uid(raw, raw_len);
}

bool RFID_get_target_tag_hex(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }

    uint8_t raw[10];
    size_t raw_len = 0;
    if (!RFID_get_saved_uid(raw, &raw_len) || raw_len == 0) {
        buf[0] = '\0';
        return false;
    }

    size_t pos = 0;
    for (size_t i = 0; i < raw_len && pos + 3 < len; ++i) {
        int written = snprintf(buf + pos, len - pos, "%02X", raw[i]);
        if (written < 0) {
            break;
        }
        pos += (size_t)written;
    }
    buf[pos] = '\0';
    return true;
}

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

static void flash_indicator(bool match, int count)
{
    if (count <= 0) {
        rgb_set_color(false, false, false);
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (match) {
            rgb_set_color(false, true, false);
        } else {
            rgb_set_color(true, false, false);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        rgb_set_color(false, false, false);
        if (i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Task: initialize PN532 and continuously scan for tags
static void pn532_scan_task(void *arg)
{
    // Configure LED GPIOs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RGB_RED_GPIO) | (1ULL << RGB_GREEN_GPIO) | (1ULL << RGB_BLUE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    rgb_set_color(false, false, false);

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
        uint8_t tmp_saved[10]; size_t tmp_saved_len = 0;
        bool has_target = RFID_get_saved_uid(tmp_saved, &tmp_saved_len);

        uid_len = sizeof(uid);
        esp_err_t r = pn532_read_passive_target_id(io_handle, PN532_BRTY_ISO14443A_106KBPS, uid, &uid_len, 1000);
        if (r == ESP_OK) {
            RFID_set_last_uid(uid, uid_len);
            ESP_LOGI(TAG, "Tag detected len=%d", uid_len);

            bool matches = has_target && tmp_saved_len > 0 && uid_equal(uid, uid_len, tmp_saved, tmp_saved_len);
            MQTT_RPi_publish_status(matches);

            int flash_count = RFID_get_flash_count();
            if (has_target && flash_count > 0) {
                flash_indicator(matches, flash_count);
            } else if (matches) {
                rgb_set_color(false, true, false);
                vTaskDelay(pdMS_TO_TICKS(500));
                rgb_set_color(false, false, false);
            } else if (has_target) {
                rgb_set_color(true, false, false);
                vTaskDelay(pdMS_TO_TICKS(500));
                rgb_set_color(false, false, false);
            } else {
                rgb_set_color(false, false, false);
            }
        } else {
            RFID_clear_last_uid();
            rgb_set_color(false, false, false);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

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
