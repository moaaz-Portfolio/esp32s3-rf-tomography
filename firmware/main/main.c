#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "lwip/inet.h"

#define CSI_MAX_BYTES 256
#define QUEUE_DEPTH 24
#define TAG "RF_TOMO"

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

    /* Drop when busy: never block the Wi-Fi driver callback. */
    (void)xQueueSend(s_csi_queue, &packet, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected; CSI capture active");
    }
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

static void start_wifi_csi(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_TOMO_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_TOMO_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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

    /* Promiscuous mode lets the station observe additional compatible frames. */
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize lwIP/TCP-IP before creating any BSD socket. */
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
    start_wifi_csi();
    ESP_LOGI(TAG, "RF Tomography CSI streamer started");
}
