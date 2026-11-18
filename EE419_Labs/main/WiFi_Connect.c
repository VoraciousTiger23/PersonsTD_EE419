#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "WiFi_Connect.h"

#define NVS_NAMESPACE      "wifi"
#define NVS_KEY_SSID       "wifissid"
#define NVS_KEY_PASSWORD   "wifipass"
#define AP_SSID            "S3_Setup"
#define MAX_RETRY_COUNT    5

static int s_retry_count = 0;
static const char *TAG = "WiFi_Connect";
static httpd_handle_t g_server = NULL;
static esp_netif_t *g_sta_netif = NULL;
static esp_netif_t *g_ap_netif = NULL;

static void ensure_default_wifi_netifs(void)
{
    if (g_sta_netif == NULL) {
        g_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (g_ap_netif == NULL) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
    }
}

static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t save_handler(httpd_req_t *req);
static esp_err_t reset_handler(httpd_req_t *req);
static void start_setup_ap(void);

static esp_err_t clear_saved_creds_from_nvs(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_key(handle, NVS_KEY_SSID);
    nvs_erase_key(handle, NVS_KEY_PASSWORD);
    err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

static void start_setup_ap(void)
{
    ESP_LOGI(TAG, "Starting Wi-Fi AP for setup mode");

    ensure_default_wifi_netifs();

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", AP_SSID);
    ap_config.ap.ssid_len = strlen(AP_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.password[0] = '\0';
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (g_server == NULL && httpd_start(&g_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };

        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_handler,
            .user_ctx = NULL
        };

        httpd_uri_t reset_uri = {
            .uri = "/reset",
            .method = HTTP_POST,
            .handler = reset_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(g_server, &root_uri);
        httpd_register_uri_handler(g_server, &save_uri);
        httpd_register_uri_handler(g_server, &reset_uri);
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Connecting to WiFi...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_retry_count < MAX_RETRY_COUNT)
                {
                    s_retry_count++;

                    ESP_LOGW(
                        TAG,
                        "Disconnected. Retry %d/%d",
                        s_retry_count,
                        MAX_RETRY_COUNT);

                    esp_wifi_connect();
                }
                else
                {
                    ESP_LOGE(
                        TAG,
                        "Failed after %d attempts. Starting setup AP.",
                        MAX_RETRY_COUNT);

                    esp_wifi_stop();

                    start_setup_ap();
                }
                break;
        }
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        s_retry_count = 0;

        ESP_LOGI(
            TAG,
            "Connected. IP: " IPSTR,
            IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t save_creds_to_nvs(
    const char *ssid,
    const char *password)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t read_creds_from_nvs(
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        return err;
    }

    size_t required_ssid = ssid_size;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &required_ssid);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    size_t required_password = password_size;
    err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &required_password);
    nvs_close(handle);
    return err;
}

static void extract_form_value(
    const char *body,
    const char *key,
    char *output,
    size_t output_size)
{
    const char *start = strstr(body, key);

    if (start == NULL) {
        output[0] = '\0';
        return;
    }

    start += strlen(key);
    const char *eq = strchr(start, '=');
    if (eq == NULL) {
        output[0] = '\0';
        return;
    }

    const char *src = eq + 1;
    size_t out_i = 0;

    while (*src && *src != '&' && out_i < output_size - 1) {
        if (*src == '+') {
            output[out_i++] = ' ';
            src++;
        } else if (*src == '%' &&
                   isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            output[out_i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            output[out_i++] = *src++;
        }
    }

    output[out_i] = '\0';
}

static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html>"
        "<html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi Setup</title>"
        "</head><body style='font-family:Arial;padding:24px;'>"
        "<h2>ESP32 WiFi Setup</h2>"
        "<form method='post' action='/save'>"
        "<input type='text' name='ssid' maxlength='32' placeholder='WiFi Name (SSID)' style='width:100%;padding:12px;margin-bottom:12px'>"
        "<input type='password' name='password' maxlength='63' placeholder='Password' style='width:100%;padding:12px;margin-bottom:12px'>"
        "<button type='submit'>Save and Connect</button>"
        "</form>"
        "<form method='post' action='/reset' style='margin-top:16px;'>"
        "<button type='submit'>Disconnect WiFi / Reset</button>"
        "</form>"
        "</body></html>";

    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    esp_wifi_disconnect();
    esp_wifi_stop();

    esp_err_t err = clear_saved_creds_from_nvs();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Unable to clear saved WiFi settings");
        return err;
    }

    ESP_LOGW(TAG, "WiFi reset requested. Clearing saved credentials.");
    httpd_resp_sendstr(req, "WiFi settings reset. Setup AP restarted.");

    start_setup_ap();
    return ESP_OK;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);

    if (received <= 0) {
        return ESP_FAIL;
    }

    body[received] = '\0';

    char ssid[33] = {0};
    char password[64] = {0};

    extract_form_value(body, "ssid", ssid, sizeof(ssid));
    extract_form_value(body, "password", password, sizeof(password));

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "SSID missing");
        return ESP_FAIL;
    }

    esp_err_t err = save_creds_to_nvs(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Unable to save credentials");
        return err;
    }

    ESP_LOGI(TAG, "Saved SSID '%s' to NVS", ssid);
    httpd_resp_sendstr(req, "Credentials saved. Rebooting...");

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static void connect_to_saved_wifi(void)
{
    char ssid[33] = {0};
    char password[64] = {0};

    esp_err_t err = read_creds_from_nvs(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved credentials found");
        start_setup_ap();
        return;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    ensure_default_wifi_netifs();

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void WiFi_Connect(void)
{
    ensure_default_wifi_netifs();

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL));

    connect_to_saved_wifi();
}