#include "MQTT_RPi.h"

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
#define MQTT_STATUS_TOPIC "lab3/personstd-esp32s3/status"
#define MQTT_UNREGISTER_TOPIC "lab3/unregister"
#define MQTT_BROKER_HOST "NEB426.local"

static esp_mqtt_client_handle_t g_client = NULL;

void MQTT_RPi_publish_status(bool targetFound)
{
    if (g_client == NULL) {
        return;
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "targetFound:%s", targetFound ? "true" : "false");
    int msg_id = esp_mqtt_client_publish(g_client, MQTT_STATUS_TOPIC, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Status publish msg_id=%d payload=%s", msg_id, payload);
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

    char *tag = strstr(text, "targetTag");
    char *count = strstr(text, "flashCount");
    if (!tag && !count) {
        ESP_LOGW(TAG, "Broker payload missing targetTag/flashCount: %s", text);
        return;
    }

    if (tag) {
        tag = strchr(tag, ':');
        if (tag) {
            tag++;
            while (*tag == ' ' || *tag == '\t' || *tag == '"' || *tag == '\r' || *tag == '\n' || *tag == ',') {
                tag++;
            }
            size_t tag_len = strcspn(tag, "\r\n\" ,}");
            char tag_buf[32] = {0};
            if (tag_len > 0 && tag_len < sizeof(tag_buf)) {
                snprintf(tag_buf, sizeof(tag_buf), "%.*s", (int)tag_len, tag);
                RFID_set_target_tag_hex(tag_buf);
                ESP_LOGI(TAG, "Set target tag from broker: %s", tag_buf);
            }
        }
    }

    if (count) {
        count = strchr(count, ':');
        if (count) {
            count++;
            while (*count == ' ' || *count == '\t' || *count == '\r' || *count == '\n' || *count == ',') {
                count++;
            }
            char *endptr = NULL;
            long parsed = strtol(count, &endptr, 10);
            if (parsed >= 0) {
                RFID_set_flash_count((int)parsed);
                ESP_LOGI(TAG, "Set flash count from broker: %d", (int)parsed);
            }
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            char payload[128];
            snprintf(payload, sizeof(payload), "deviceId:%s", DEVICE_ID);
            int msg_id = esp_mqtt_client_publish(g_client, MQTT_REGISTER_TOPIC, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Register publish msg_id=%d payload=%s", msg_id, payload);
            break;

        case MQTT_EVENT_DATA: {
            char *topic = event->topic ? event->topic : "";
            if (strncmp(topic, MQTT_REGISTER_TOPIC, event->topic_len) == 0) {
                char payload_buf[256];
                size_t used = event->data_len < sizeof(payload_buf) - 1 ? event->data_len : sizeof(payload_buf) - 1;
                memcpy(payload_buf, event->data, used);
                payload_buf[used] = '\0';
                ESP_LOGI(TAG, "Received register response: %s", payload_buf);
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

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname = MQTT_BROKER_HOST,
                .port = 1883,
            },
        },
        .session = {
            .last_will = {
                .topic = MQTT_UNREGISTER_TOPIC,
                .msg = "deviceId:personstd-esp32s3",
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
    resolve_host_and_connect();
}
