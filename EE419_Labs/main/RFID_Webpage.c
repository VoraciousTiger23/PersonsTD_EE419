#include "RFID_Webpage.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "lwip/apps/sntp.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "RFID_Sensor.h"

static const char *TAG = "RFID_Webpage";

#define NVS_NAMESPACE "rfid"
#define NVS_KEY_SAVED_UID "saved_uid"

static void uid_to_hex(const uint8_t *uid, size_t len, char *out, size_t out_len)
{
    if (!uid || len == 0) {
        if (out_len) out[0] = '\0';
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len && (pos + 3) < out_len; ++i) {
        int n = snprintf(out + pos, out_len - pos, "%02X", uid[i]);
        if (n < 0) break;
        pos += (size_t)n;
        if (i + 1 < len && (pos + 1) < out_len) {
            out[pos++] = ':';
            out[pos] = '\0';
        }
    }
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char timebuf[64] = {0};
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // read saved (target) UID from in-memory cache populated by RFID_Sensor
    char target_str[64] = {0};
    uint8_t saved_buf[10];
    size_t saved_len = 0;
    if (RFID_get_saved_uid(saved_buf, &saved_len)) {
        uid_to_hex(saved_buf, saved_len, target_str, sizeof(target_str));
    }

    // read current detected UID
    char current_str[64] = {0};
    uint8_t curbuf[10];
    size_t curlen = 0;
    if (RFID_get_last_uid(curbuf, &curlen)) {
        uid_to_hex(curbuf, curlen, current_str, sizeof(current_str));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    char resp[512];
    snprintf(resp, sizeof(resp), "{\"time\":\"%s\",\"target\":\"%s\",\"current\":\"%s\"}",
             timebuf, target_str, current_str);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    nvs_handle_t nvs_handle;
    esp_err_t res = ESP_FAIL;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        res = nvs_erase_key(nvs_handle, NVS_KEY_SAVED_UID);
        if (res == ESP_OK) {
            nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    if (res == ESP_OK) {
        RFID_clear_last_uid();
        RFID_clear_saved_uid();
        /* Ensure LED returns to blue when target is reset */
        rgb_set_color(false, false, true);
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset");
    return ESP_FAIL;
}

/* Note: manual set-target endpoint removed; the target is auto-saved on first detection */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>RFID Status</title>"
        "<style>body{font-family:Arial;margin:20px;}button{padding:8px 12px}</style>"
        "</head><body>"
        "<h2>RFID Sensor</h2>"
        "<div><strong>Time:</strong> <span id=\"time\">-</span></div>"
        "<div><strong>Target Tag:</strong> <span id=\"target\">-</span></div>"
        "<div><strong>Current Tag:</strong> <span id=\"current\">-</span></div>"
        "<div style=\"margin-top:12px;\"><button id=\"reset\">Reset Target Tag</button></div>"
        "<script>async function update(){try{let r=await fetch('/status',{cache:'no-store'});let j=await r.json();document.getElementById('time').innerText=j.time||'-';document.getElementById('target').innerText=j.target||'-';document.getElementById('current').innerText=j.current||'-';}catch(e){console.log(e);}setTimeout(update,1000);}document.getElementById('reset').addEventListener('click',async()=>{await fetch('/reset',{method:'POST'});});update();</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

void RFID_Webpage_init(void)
{
    // initialize SNTP to provide real time
    ESP_LOGI(TAG, "Initializing SNTP");
    /* Use lwIP SNTP API name */
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // init mDNS
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("personstd-esp32s3"));
    ESP_ERROR_CHECK(mdns_instance_name_set("personstd-esp32s3"));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    // start web server
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status);

    httpd_uri_t reset = {
        .uri = "/reset",
        .method = HTTP_POST,
        .handler = reset_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &reset);

    // manual /set_target endpoint intentionally not registered; automatic target selection enabled

    ESP_LOGI(TAG, "Web UI available at http://personstd-esp32s3.local/");
}
