#include "MQTT_RPi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "mdns.h"

#include "RFID_Sensor.h"

static const char *TAG = "MQTT_RPi";

#define DEVICE_ID "personstd-esp32s3"
#define MQTT_REGISTER_TOPIC "lab3/register"
#define MQTT_COMMAND_TOPIC "lab3/" DEVICE_ID "/cmd"
#define MQTT_STATUS_TOPIC "lab3/" DEVICE_ID "/status"
#define MQTT_UNREGISTER_TOPIC "lab3/unregister"
#define MQTT_BROKER_HOST "NEB426.local"

static esp_mqtt_client_handle_t g_client = NULL;

void MQTT_RPi_publish_status(bool targetFound)
{
    if (g_client == NULL) {
        return;
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"targetFound\":%s}", targetFound ? "true" : "false");
    int msg_id = esp_mqtt_client_publish(g_client, MQTT_STATUS_TOPIC, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Status publish msg_id=%d payload=%s", msg_id, payload);
}

static bool extract_json_string_value(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return false;
    }

    char key_pattern[32];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
    const char *key_pos = strstr(json, key_pattern);
    if (!key_pos) {
        return false;
    }

    const char *value_pos = strchr(key_pos + strlen(key_pattern), ':');
    if (!value_pos) {
        return false;
    }
    value_pos++;

    while (*value_pos == ' ' || *value_pos == '\t' || *value_pos == '\r' || *value_pos == '\n' || *value_pos == '"') {
        value_pos++;
    }

    const char *end = value_pos;
    while (*end && *end != '"' && *end != ',' && *end != '}' && *end != '\r' && *end != '\n') {
        end++;
    }

    size_t value_len = (size_t)(end - value_pos);
    if (value_len == 0 || value_len >= out_size) {
        return false;
    }

    memcpy(out, value_pos, value_len);
    out[value_len] = '\0';
    return true;
}

static bool extract_json_int_value(const char *json, const char *key, int *out_value)
{
    if (!json || !key || !out_value) {
        return false;
    }

    char key_pattern[32];
    snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", key);
    const char *key_pos = strstr(json, key_pattern);
    if (!key_pos) {
        return false;
    }

    const char *value_pos = strchr(key_pos + strlen(key_pattern), ':');
    if (!value_pos) {
        return false;
    }
    value_pos++;

    while (*value_pos == ' ' || *value_pos == '\t' || *value_pos == '\r' || *value_pos == '\n') {
        value_pos++;
    }

    char *endptr = NULL;
    long parsed = strtol(value_pos, &endptr, 10);
    if (endptr == value_pos) {
        return false;
    }

    *out_value = (int)parsed;
    return true;
}

static void parse_target_tag(const char *payload, size_t len)
{
    if (!payload || len == 0) {
        RFID_clear_saved_uid();
        RFID_set_flash_count(0);
        return;
    }

    char text[256];
    size_t copy_len = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
    memcpy(text, payload, copy_len);
    text[copy_len] = '\0';

    char tag_buf[32] = {0};
    int flash_count = -1;

    bool has_tag = extract_json_string_value(text, "targetTag", tag_buf, sizeof(tag_buf));
    bool has_count = extract_json_int_value(text, "flashCount", &flash_count);

    if (!has_tag && !has_count) {
        ESP_LOGW(TAG, "Broker payload missing targetTag/flashCount: %s", text);
        return;
    }

    if (has_tag) {
        size_t tag_len = strlen(tag_buf);
        if (tag_len == 8) {
            bool valid_hex = true;
            for (size_t i = 0; i < tag_len; i++) {
                if (!isxdigit((unsigned char)tag_buf[i])) {
                    valid_hex = false;
                    break;
                }
            }

            if (valid_hex) {
                RFID_set_target_tag_hex(tag_buf);
                ESP_LOGI(TAG, "Set target tag from broker: %s", tag_buf);
            } else {
                ESP_LOGW(TAG, "Invalid targetTag value from broker: %s", tag_buf);
            }
        } else {
            ESP_LOGW(TAG, "Unexpected targetTag length from broker: %zu", tag_len);
        }
    }

    if (has_count) {
        if (flash_count >= 1 && flash_count <= 10) {
            RFID_set_flash_count(flash_count);
            ESP_LOGI(TAG, "Set flash count from broker: %d", flash_count);
        } else {
            ESP_LOGW(TAG, "Flash count out of range from broker: %d", flash_count);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");

            int subscribe_msg_id = esp_mqtt_client_subscribe(g_client, MQTT_COMMAND_TOPIC, 1);
            if (subscribe_msg_id == -1) {
                ESP_LOGE(TAG, "Failed to subscribe to %s", MQTT_COMMAND_TOPIC);
                break;
            }
            ESP_LOGI(TAG, "Subscribed to %s (msg_id=%d)", MQTT_COMMAND_TOPIC, subscribe_msg_id);

            char payload[128];
            snprintf(payload, sizeof(payload), "{\"deviceId\":\"%s\"}", DEVICE_ID);
            int register_msg_id = esp_mqtt_client_publish(g_client, MQTT_REGISTER_TOPIC, payload, 0, 1, 0);
            if (register_msg_id == -1) {
                ESP_LOGE(TAG, "Failed to publish registration payload to %s", MQTT_REGISTER_TOPIC);
                break;
            }
            ESP_LOGI(TAG, "Register publish msg_id=%d payload=%s", register_msg_id, payload);
            break;

        case MQTT_EVENT_DATA: {
            char *topic = event->topic ? event->topic : "";
            if (strncmp(topic, MQTT_COMMAND_TOPIC, event->topic_len) == 0) {
                char payload_buf[256];
                size_t used = event->data_len < sizeof(payload_buf) - 1 ? event->data_len : sizeof(payload_buf) - 1;
                memcpy(payload_buf, event->data, used);
                payload_buf[used] = '\0';
                ESP_LOGI(TAG, "Received command payload: %s", payload_buf);
                parse_target_tag(payload_buf, used);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

static void resolve_host_and_connect(void)
{
    ESP_LOGI(TAG, "Resolving broker host %s via mDNS", MQTT_BROKER_HOST);

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init failed: %d", err);
    }

    const char *broker_uri = "mqtt://NEB426.local:1883";

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = broker_uri,
            },
        },
        .session = {
            .last_will = {
                .topic = MQTT_UNREGISTER_TOPIC,
                .msg = "{\"deviceId\":\"personstd-esp32s3\"}",
                .qos = 1,
                .retain = 0,
            },
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        },
        .buffer = {
            .size = 2048,
        },
    };

    g_client = esp_mqtt_client_init(&mqtt_cfg);
    if (g_client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }

    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    err = esp_mqtt_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", err);
    }
}

void MQTT_RPi_init(void)
{
    ESP_LOGI(TAG, "MQTT_RPi_init: starting broker resolution for %s", "NEB426.local");
    resolve_host_and_connect();
}
