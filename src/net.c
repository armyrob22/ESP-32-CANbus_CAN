// Network supervisor: hotspot WiFi -> SNTP -> WireGuard tunnel to server2.
//
// Order matters: WireGuard handshakes carry a TAI64N timestamp and the
// server rejects one that isn't newer than the last it saw, so after a
// reboot the clock must be real (SNTP) before the first handshake.

#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wireguard.h"
#include "net.h"

static const char *TAG = "net";

#define BIT_GOT_IP BIT0

static EventGroupHandle_t s_ev;
static atomic_bool s_time_valid;
static atomic_bool s_link_up;

static wireguard_config_t s_wg_cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t    s_wg_ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();
static bool s_wg_inited;
static bool s_wg_connected;

// ---------------------------------------------------------------- events

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        atomic_store(&s_link_up, false);
        ESP_LOGW(TAG, "wifi disconnected (reason %d), retrying", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        atomic_store(&s_link_up, false);
    }
}

static void on_time_sync(struct timeval *tv)
{
    if (!atomic_exchange(&s_time_valid, true)) {
        ESP_LOGI(TAG, "time synced");
    }
}

// ---------------------------------------------------------------- wireguard
// lwIP isn't thread safe, so every WireGuard call runs inside the tcpip
// thread via esp_netif_tcpip_exec().

static esp_err_t wg_connect_cb(void *ctx)    { return esp_wireguard_connect(&s_wg_ctx); }
static esp_err_t wg_disconnect_cb(void *ctx) { return esp_wireguard_disconnect(&s_wg_ctx); }
static esp_err_t wg_peer_up_cb(void *ctx)    { return esp_wireguard_peer_is_up(&s_wg_ctx); }
static esp_err_t wg_allow_cb(void *ctx)
{
    return esp_wireguard_add_allowed_ip(&s_wg_ctx, CONFIG_KCAN_SERVER_IP, "255.255.255.255");
}

static bool wg_bring_up(void)
{
    if (!s_wg_inited) {
        s_wg_cfg.private_key          = CONFIG_KCAN_WG_PRIVATE_KEY;
        s_wg_cfg.public_key           = CONFIG_KCAN_WG_PEER_PUBLIC_KEY;
        s_wg_cfg.preshared_key        = strlen(CONFIG_KCAN_WG_PRESHARED_KEY) ? CONFIG_KCAN_WG_PRESHARED_KEY : NULL;
        s_wg_cfg.address              = CONFIG_KCAN_WG_LOCAL_IP;
        s_wg_cfg.netmask              = CONFIG_KCAN_WG_LOCAL_NETMASK;
        s_wg_cfg.endpoint             = CONFIG_KCAN_WG_ENDPOINT;
        s_wg_cfg.port                 = CONFIG_KCAN_WG_ENDPOINT_PORT;
        s_wg_cfg.persistent_keepalive = CONFIG_KCAN_WG_KEEPALIVE_S;
        esp_err_t err = esp_wireguard_init(&s_wg_cfg, &s_wg_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wireguard init: %s", esp_err_to_name(err));
            return false;
        }
        s_wg_inited = true;
    }

    // connect returns ESP_ERR_RETRY while the endpoint hostname resolves.
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 20; i++) {
        err = esp_netif_tcpip_exec(wg_connect_cb, NULL);
        if (err != ESP_ERR_RETRY) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wireguard connect: %s", esp_err_to_name(err));
        if (s_wg_ctx.netif) esp_netif_tcpip_exec(wg_disconnect_cb, NULL);
        return false;
    }

    // Outbound packets are routed to the peer whose allowed IPs match the
    // destination, so server2's tunnel address has to be on the list.
    err = esp_netif_tcpip_exec(wg_allow_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add allowed ip: %s", esp_err_to_name(err));
    }

    for (int i = 0; i < 30; i++) {
        if (esp_netif_tcpip_exec(wg_peer_up_cb, NULL) == ESP_OK) {
            ESP_LOGI(TAG, "wireguard peer up");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "no handshake after 15 s (check keys, port forward, endpoint)");
    esp_netif_tcpip_exec(wg_disconnect_cb, NULL);
    return false;
}

static void wg_tear_down(void)
{
    if (s_wg_connected && s_wg_ctx.netif) {
        esp_netif_tcpip_exec(wg_disconnect_cb, NULL);
    }
    s_wg_connected = false;
    atomic_store(&s_link_up, false);
}

// ---------------------------------------------------------------- supervisor

static void net_task(void *arg)
{
    bool sntp_started = false;
    int64_t peer_down_since = 0;

    for (;;) {
        xEventGroupWaitBits(s_ev, BIT_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);

        if (!sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_KCAN_NTP_SERVER);
            cfg.sync_cb = on_time_sync;
            esp_netif_sntp_init(&cfg);
            sntp_started = true;
        }
        if (!atomic_load(&s_time_valid)) {
            esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
            if (!atomic_load(&s_time_valid)) {
                ESP_LOGW(TAG, "waiting for SNTP before WireGuard");
                continue;
            }
        }

        if (!s_wg_connected) {
            if (wg_bring_up()) {
                s_wg_connected = true;
                peer_down_since = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        // Steady state: watch the link.
        bool ip = xEventGroupGetBits(s_ev) & BIT_GOT_IP;
        bool peer = esp_netif_tcpip_exec(wg_peer_up_cb, NULL) == ESP_OK;
        atomic_store(&s_link_up, ip && peer);

        if (!ip) {
            ESP_LOGW(TAG, "lost IP, dropping tunnel");
            wg_tear_down();
        } else if (!peer) {
            int64_t now = esp_timer_get_time();
            if (!peer_down_since) peer_down_since = now;
            // Reconnecting re-resolves the endpoint, which also covers a
            // changed home IP behind DDNS.
            if (now - peer_down_since > 60 * 1000000LL) {
                ESP_LOGW(TAG, "peer down 60 s, reconnecting");
                wg_tear_down();
            }
        } else {
            peer_down_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------------------------------------------------------- public

void net_start(void)
{
    s_ev = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_wifi, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_KCAN_WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, CONFIG_KCAN_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep adds up to a DTIM interval of latency per packet.
    esp_wifi_set_ps(WIFI_PS_NONE);

    xTaskCreatePinnedToCore(net_task, "net", 6144, NULL, 5, NULL, 0);
}

bool net_link_up(void)    { return atomic_load(&s_link_up); }
bool net_time_valid(void) { return atomic_load(&s_time_valid); }

int8_t net_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
