#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "lwip/inet.h"

#define CSI_MAX_BYTES 256
#define QUEUE_DEPTH 24
#define TAG "RF_TOMO"
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "password"
#define SETUP_AP_SSID "RF-TOMO-SETUP"
#define SETUP_AP_PASSWORD "rf-tomo-setup"

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    uint16_t len;
    int8_t csi[CSI_MAX_BYTES];
    int64_t timestamp_us;
} csi_packet_t;

static QueueHandle_t s_csi_queue;
static int s_sock = -1;
static struct sockaddr_in s_collector;
static bool s_has_credentials = false;
static bool s_csi_enabled = false;
static httpd_handle_t s_http_server = NULL;

static const char SETUP_PAGE[] =
"<!doctype html><html lang='ar' dir='rtl'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta name='theme-color' content='#080b13'><title>RF TOMOGRAPHY SETUP</title>"
"<style>*{box-sizing:border-box}body{margin:0;background:#080b13;color:#eef5ff;font-family:Arial,sans-serif;padding:24px;min-height:100vh;display:grid;place-items:center}"
".card{width:100%;max-width:460px;background:#101725;border:1px solid #263449;border-radius:22px;padding:26px;box-shadow:0 20px 70px #0008}"
".tag{color:#4de3d1;font-size:12px;letter-spacing:2px}h1{font-size:27px;margin:12px 0}p{color:#9aabc0;line-height:1.9;font-size:14px}"
"label{display:block;margin:18px 0 7px;font-size:13px;color:#c9d6e6}input{width:100%;padding:14px;border-radius:11px;border:1px solid #34465d;background:#080f1b;color:white;font-size:16px;outline:none}"
"input:focus{border-color:#4de3d1}button{width:100%;padding:14px;border:0;border-radius:11px;background:#4de3d1;color:#061016;font-size:15px;font-weight:bold;margin-top:20px;cursor:pointer}"
".secondary{background:#263449;color:#eef5ff;margin-top:10px}#msg{min-height:28px;color:#4de3d1;font-size:13px;margin-top:14px}.foot{font-size:11px;color:#6f8199;text-align:center;margin-top:20px}</style></head>"
"<body><main class='card'><div class='tag'>RF TOMOGRAPHY / DEVICE SETUP</div><h1>إعداد اتصال الجهاز</h1>"
"<p>اكتب بيانات شبكة الراوتر بتردد 2.4GHz. الجهاز هيحفظها ويحاول الاتصال تلقائيًا، وشبكة الإعدادات هتفضل متاحة.</p>"
"<form id='wifi'><label>اسم شبكة Wi-Fi (SSID)</label><input name='ssid' maxlength='32' required autocomplete='off' placeholder='اسم الشبكة'>"
"<label>كلمة مرور الشبكة</label><input name='password' type='password' maxlength='64' autocomplete='new-password' placeholder='كلمة المرور (اتركها فارغة للشبكات المفتوحة)'>"
"<button type='submit'>حفظ والاتصال</button></form><div id='msg'></div>"
"<form method='POST' action='/reset' onsubmit=\"return confirm('مسح بيانات الشبكة المحفوظة؟')\"><button class='secondary' type='submit'>إعادة ضبط Wi-Fi</button></form>"
"<div class='foot'>اتصل بعنوان 192.168.4.1 في أي وقت لإعداد الجهاز</div></main>"
"<script>document.getElementById('wifi').addEventListener('submit',async e=>{e.preventDefault();let m=document.getElementById('msg');m.textContent='جاري حفظ البيانات والاتصال...';try{let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))});m.textContent=await r.text()}catch(_){m.textContent='تعذر إرسال البيانات. حاول مرة أخرى.'}});</script></body></html>";

static void csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || info->len <= 0 || !s_csi_queue) return;

    csi_packet_t packet = {0};
    memcpy(packet.mac, info->mac, sizeof(packet.mac));
    packet.rssi = info->rx_ctrl.rssi;
    packet.channel = info->rx_ctrl.channel;
    packet.len = (uint16_t)(info->len > CSI_MAX_BYTES ? CSI_MAX_BYTES : info->len);
    memcpy(packet.csi, info->buf, packet.len);
    packet.timestamp_us = esp_timer_get_time();
    (void)xQueueSend(s_csi_queue, &packet, 0);
}

static void enable_csi_capture(void)
{
    if (s_csi_enabled) return;

    wifi_csi_config_t csi_cfg = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    s_csi_enabled = true;
    ESP_LOGI(TAG, "CSI capture enabled");
}

static void udp_task(void *arg)
{
    (void)arg;
    csi_packet_t packet;
    char json[1800];

    while (true) {
        if (xQueueReceive(s_csi_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 packet.mac[0], packet.mac[1], packet.mac[2],
                 packet.mac[3], packet.mac[4], packet.mac[5]);

        int used = snprintf(json, sizeof(json),
            "{\"type\":\"csi\",\"mac\":\"%s\",\"rssi\":%d,"
            "\"channel\":%u,\"timestamp_us\":%" PRId64 ",\"csi\":[",
            mac, packet.rssi, packet.channel, packet.timestamp_us);

        for (int i = 0; i < packet.len && used > 0 && used < (int)sizeof(json) - 16; ++i) {
            used += snprintf(json + used, sizeof(json) - (size_t)used,
                             "%s%d", i ? "," : "", packet.csi[i]);
        }
        if (used > 0 && used < (int)sizeof(json) - 3) {
            json[used++] = ']'; json[used++] = '}';
            (void)sendto(s_sock, json, (size_t)used, 0,
                         (struct sockaddr *)&s_collector, sizeof(s_collector));
        }
    }
}

static bool load_wifi_credentials(char *ssid, size_t ssid_len,
                                  char *password, size_t password_len)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t slen = ssid_len;
    size_t plen = password_len;
    esp_err_t sret = nvs_get_str(handle, WIFI_NVS_SSID_KEY, ssid, &slen);
    esp_err_t pret = nvs_get_str(handle, WIFI_NVS_PASS_KEY, password, &plen);
    nvs_close(handle);
    return sret == ESP_OK && pret == ESP_OK && ssid[0] != '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void form_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_len; ++i) {
        if (src[i] == '+' ) {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[out++] = src[i];
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool get_form_value(const char *body, const char *key,
                           char *out, size_t out_len)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *start = strstr(body, pattern);
    if (!start) return false;
    start += strlen(pattern);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    form_decode(out, out_len, start, len);
    return true;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, SETUP_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    char body[257] = {0};
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not read form");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    if (!get_form_value(body, "ssid", ssid, sizeof(ssid)) ||
        !get_form_value(body, "password", password, sizeof(password)) ||
        ssid[0] == '\0' || strlen(ssid) > 32 || strlen(password) > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID or password is invalid");
        return ESP_FAIL;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    bool nvs_opened = (err == ESP_OK);
    if (err == ESP_OK) err = nvs_set_str(handle, WIFI_NVS_SSID_KEY, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, WIFI_NVS_PASS_KEY, password);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (nvs_opened) nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Wi-Fi credentials: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
        return ESP_FAIL;
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    s_has_credentials = true;
    s_csi_enabled = false;
    (void)esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    esp_err_t connect_err = esp_wifi_connect();

    ESP_LOGI(TAG, "Saved Wi-Fi credentials for SSID: %s", ssid);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (connect_err == ESP_OK) {
        return httpd_resp_sendstr(req, "تم حفظ البيانات. الجهاز بيحاول الاتصال بالراوتر الآن. راقب Serial Monitor.");
    }
    return httpd_resp_sendstr(req, "تم حفظ البيانات. سيعيد الجهاز محاولة الاتصال تلقائيًا.");
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, WIFI_NVS_SSID_KEY);
        nvs_erase_key(handle, WIFI_NVS_PASS_KEY);
        err = nvs_commit(handle);
        nvs_close(handle);
    }

    s_has_credentials = false;
    s_csi_enabled = false;
    (void)esp_wifi_disconnect();
    wifi_config_t empty_cfg = {0};
    (void)esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Saved Wi-Fi credentials erased; setup AP remains active");
        return httpd_resp_sendstr(req, "تم مسح بيانات الشبكة. الجهاز جاهز لإعداد شبكة جديدة. ارجع للصفحة الرئيسية.");
    }
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not erase settings");
}

static void start_setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start setup web server");
        return;
    }

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = NULL};
    httpd_uri_t reset = {.uri = "/reset", .method = HTTP_POST, .handler = reset_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_http_server, &root);
    httpd_register_uri_handler(s_http_server, &save);
    httpd_register_uri_handler(s_http_server, &reset);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_credentials) (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_csi_enabled = false;
        if (s_has_credentials) {
            ESP_LOGW(TAG, "Router disconnected; retrying Wi-Fi connection");
            vTaskDelay(pdMS_TO_TICKS(250));
            (void)esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "No router credentials saved; setup AP is ready");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected to router. STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        enable_csi_capture();
    }
}

static void start_wifi_and_portal(void)
{
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    char ssid[33] = {0};
    char password[65] = {0};
    s_has_credentials = load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, SETUP_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, SETUP_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(SETUP_AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    wifi_config_t sta_cfg = {0};
    if (s_has_credentials) {
        strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        ESP_LOGI(TAG, "Loaded saved Wi-Fi network: %s", ssid);
    } else {
        ESP_LOGW(TAG, "No saved Wi-Fi network. Use the setup AP to configure this device.");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    start_setup_server();

    ESP_LOGI(TAG, "Setup Wi-Fi: %s", SETUP_AP_SSID);
    ESP_LOGI(TAG, "Setup password: %s", SETUP_AP_PASSWORD);
    ESP_LOGI(TAG, "Open http://192.168.4.1/ to configure router Wi-Fi");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_csi_queue = xQueueCreate(QUEUE_DEPTH, sizeof(csi_packet_t));
    if (!s_csi_queue) {
        ESP_LOGE(TAG, "Could not allocate CSI queue");
        return;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Could not create UDP socket");
        return;
    }
    memset(&s_collector, 0, sizeof(s_collector));
    s_collector.sin_family = AF_INET;
    s_collector.sin_port = htons(CONFIG_TOMO_COLLECTOR_PORT);
    s_collector.sin_addr.s_addr = inet_addr(CONFIG_TOMO_COLLECTOR_IP);

    xTaskCreate(udp_task, "csi_udp", 6144, NULL, 5, NULL);
    start_wifi_and_portal();
    ESP_LOGI(TAG, "RF Tomography CSI node started; configure Wi-Fi through setup AP");
}
