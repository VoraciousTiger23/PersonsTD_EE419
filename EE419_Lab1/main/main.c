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

#define WIFI_SSID "NEB426"
#define WIFI_SETUP_SSID "NEB426_Setup"
#define WIFI_SETUP_CHANNEL 1
#define NVS_NAMESPACE "wifi"
#define NVS_KEY_PASSWORD "neb426_pass"

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
        {false, false, false} // Off
    };

    while (1) {
        for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
            rgb_set_color(colors[i].red, colors[i].green, colors[i].blue);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static esp_err_t save_password_to_nvs(const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t read_password_from_nvs(char *password, size_t password_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &password_size);
    nvs_close(handle);
    return err;
}

static void extract_form_value(const char *body, const char *key, char *output, size_t output_size)
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

    const char *amp = strchr(eq + 1, '&');
    size_t len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
    if (len >= output_size) {
        len = output_size - 1;
    }
    memcpy(output, eq + 1, len);
    output[len] = '\0';
}

static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>NEB426 Setup</title></head><body style='font-family:Arial,sans-serif;padding:24px;background:#111;color:#fff'>"
        "<h2>NEB426 Setup</h2>"
        "<p>Enter the Wi-Fi password for the NEB426 network.</p>"
        "<form method='POST' action='/save'>"
        "<input type='password' name='password' maxlength='63' style='width:100%;padding:12px;border-radius:8px;border:none;margin-bottom:12px' placeholder='Password'>"
        "<button type='submit' style='padding:12px 20px;border:none;border-radius:8px;background:#4CAF50;color:white'>Save and Connect</button>"
        "</form></body></html>";

    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        return ESP_FAIL;
    }
    body[received] = '\0';

    char password[64] = {0};
    extract_form_value(body, "password", password, sizeof(password));
    if (password[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Password missing");
        return ESP_FAIL;
    }

    esp_err_t err = save_password_to_nvs(password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password to NVS: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Unable to save password");
        return err;
    }

    ESP_LOGI(TAG, "Saved NEB426 password to NVS");
    httpd_resp_sendstr(req, "Password saved. Rebooting to connect to NEB426...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

    return ESP_OK;
}

static httpd_uri_t uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_handler,
    .user_ctx = NULL
};

static httpd_uri_t uri_save = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = save_handler,
    .user_ctx = NULL
};

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_save);
        return server;
    }
    return NULL;
}

static void start_setup_ap(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SETUP_SSID,
            .ssid_len = strlen(WIFI_SETUP_SSID),
            .channel = WIFI_SETUP_CHANNEL,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = ipaddr_addr("192.168.4.1");
    ip_info.netmask.addr = ipaddr_addr("255.255.255.0");
    ip_info.gw.addr = ipaddr_addr("192.168.4.1");
    esp_netif_dhcps_stop(ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    ESP_LOGI(TAG, "Setup AP started. Connect to %s and open http://192.168.4.1/", WIFI_SETUP_SSID);
    start_web_server();
}

static void connect_to_neb426(void)
{
    char password[64] = {0};
    esp_err_t err = read_password_from_nvs(password, sizeof(password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved password found in NVS: %s", esp_err_to_name(err));
        start_setup_ap();
        return;
    }

    ESP_LOGI(TAG, "Using saved password from NVS to connect to NEB426");

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    wifi_config_t sta_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = "",
        },
    };
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.password[sizeof(sta_config.sta.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    (void)sta_netif;
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

    connect_to_neb426();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
