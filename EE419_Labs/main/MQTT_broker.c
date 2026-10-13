#include "MQTT_broker.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "mdns.h"
#include "cJSON.h"

#include "RFID_Sensor.h"

static const char *TAG = "MQTT_broker";

#define DEVICE_ID "personstd-esp32s3"
#define MQTT_REGISTER_TOPIC "ee419_lab/register"
#define MQTT_COMMAND_TOPIC "ee419_lab/" DEVICE_ID "/cmd"
#define MQTT_STATUS_TOPIC "ee419_lab/" DEVICE_ID "/status"
#define MQTT_UNREGISTER_TOPIC "ee419_lab/unregister"

static esp_mqtt_client_handle_t g_client = NULL;

// x509 certificates stored externally
extern const uint8_t device_cert_pem_start[] asm("_binary_device_cert_pem_crt_start");
extern const uint8_t device_cert_pem_end[] asm("_binary_device_cert_pem_crt_end");
extern const uint8_t device_key_pem_start[] asm("_binary_device_private_key_start");
extern const uint8_t device_key_pem_end[] asm("_binary_device_private_key_end");
extern const uint8_t server_cert_pem_start[] asm("_binary_AmazonRootCA1_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_AmazonRootCA1_pem_end");

void MQTT_broker_publish_status(bool targetFound)
{
    if (g_client == NULL) {
        return;
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"targetFound\":%s}", targetFound ? "true" : "false");
    int msg_id = esp_mqtt_client_publish(g_client, MQTT_STATUS_TOPIC, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Status publish msg_id=%d payload=%s", msg_id, payload);
}

static cJSON *find_json_field(cJSON *root, const char *key)
{
    if (!root || !key) {
        return NULL;
    }

    cJSON *field = cJSON_GetObjectItemCaseSensitive(root, key);
    if (field) {
        return field;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (state && cJSON_IsObject(state)) {
        field = cJSON_GetObjectItemCaseSensitive(state, key);
        if (field) {
            return field;
        }

        cJSON *desired = cJSON_GetObjectItemCaseSensitive(state, "desired");
        if (desired && cJSON_IsObject(desired)) {
            return cJSON_GetObjectItemCaseSensitive(desired, key);
        }
    }

    cJSON *desired = cJSON_GetObjectItemCaseSensitive(root, "desired");
    if (desired && cJSON_IsObject(desired)) {
        return cJSON_GetObjectItemCaseSensitive(desired, key);
    }

    return NULL;
}

static void parse_target_tag(const char *payload, size_t len)
{
    if (!payload || len == 0) {
        RFID_clear_saved_uid();
        RFID_set_flash_count(0);
        return;
    }

    char *json_copy = malloc(len + 1);
    if (!json_copy) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        return;
    }

    memcpy(json_copy, payload, len);
    json_copy[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(json_copy, len);
    free(json_copy);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "Broker payload is not valid JSON: %s", payload);
        if (error_ptr != NULL) {
            ESP_LOGW(TAG, "JSON parse error near: %s", error_ptr);
        }
        return;
    }

    bool has_tag = false;
    bool has_count = false;
    char tag_buf[32] = {0};
    int flash_count = -1;

    cJSON *tag_item = find_json_field(root, "targetTag");
    if (tag_item && cJSON_IsString(tag_item) && tag_item->valuestring) {
        snprintf(tag_buf, sizeof(tag_buf), "%s", tag_item->valuestring);
        has_tag = true;
    }

    cJSON *count_item = find_json_field(root, "flashCount");
    if (count_item) {
        if (cJSON_IsNumber(count_item)) {
            flash_count = count_item->valueint;
            has_count = true;
        } else if (cJSON_IsString(count_item) && count_item->valuestring) {
            char *endptr = NULL;
            long parsed = strtol(count_item->valuestring, &endptr, 10);
            if (endptr != count_item->valuestring) {
                flash_count = (int)parsed;
                has_count = true;
            }
        }
    }

    if (!has_tag && !has_count) {
        ESP_LOGW(TAG, "Broker payload missing targetTag/flashCount: %s", payload);
        cJSON_Delete(root);
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
        if (flash_count >= 0 && flash_count <= 10) {
            RFID_set_flash_count(flash_count);
            ESP_LOGI(TAG, "Set flash count from broker: %d", flash_count);
        } else {
            ESP_LOGW(TAG, "Flash count out of range from broker: %d", flash_count);
        }
    }

    cJSON_Delete(root);
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
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init failed: %d", err);
    }

    const char *alpn_protos[] = {"x-amzn-mqtt-ca", NULL};

    esp_mqtt_client_config_t mqtt_cfg = {
    .broker = {
            .address = {
                .hostname = CONFIG_MQTT_BROKER,
                .port = CONFIG_MQTT_PORT,
                .transport = MQTT_TRANSPORT_OVER_SSL
            },
       .verification = {
          .certificate = (const char *) server_cert_pem_start,
          .certificate_len = 0,
          .alpn_protos = alpn_protos
       }
    },
    .credentials = {
       .username = NULL,
       .client_id = "personstd-esp32s3",
       .set_null_client_id = false,
       .authentication = {
          .password = NULL,
          .certificate = (const char*) device_cert_pem_start,
          .certificate_len = 0,
          .key = (const char*) device_key_pem_start,
          .key_len = 0,
          .key_password = NULL,
          .key_password_len = 0,
          .use_secure_element = false,
          .ds_data = NULL
       } 
    },
    .session = {
          .last_will = {
          .topic = MQTT_UNREGISTER_TOPIC,
          .msg = "{\"deviceId\":\"" DEVICE_ID "\"}",
          .qos = 0,
          .retain = false,
        },
        .keepalive = 60
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

void MQTT_broker_init(void)
{
    resolve_host_and_connect();
}
