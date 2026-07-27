#include "f_mdns.h"
#include "f_core.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *TAG           = "f_mdns";
static struct f_mdns *s_instance = NULL;

#define NVS_NAMESPACE    "mdns"
#define NVS_KEY_HOSTNAME "hostname"
#define MDNS_INSTANCE    "ESPFM Fan Controller"

struct f_mdns {
    char hostname[64];
    bool running;
    esp_event_handler_instance_t wifi_instance;
    esp_event_handler_instance_t ap_instance;
};

/* --- Hostname helpers --- */

static bool validate_hostname(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len > 63) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!islower((unsigned char)c) && !isdigit((unsigned char)c) && c != '-') return false;
    }
    if (name[0] == '-' || name[len - 1] == '-') return false;
    return true;
}

static void generate_default_hostname(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "espfm-%02x%02x", mac[4], mac[5]);
}

static void load_hostname(struct f_mdns *h)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t sizeof_hostname = sizeof(h->hostname);
        if (nvs_get_str(nvs, NVS_KEY_HOSTNAME, h->hostname, &sizeof_hostname) == ESP_OK) {
            ESP_LOGI(TAG, "Hostname from NVS: %s", h->hostname);
            nvs_close(nvs);
            return;
        }
        nvs_close(nvs);
    }
    generate_default_hostname(h->hostname, sizeof(h->hostname));
    ESP_LOGI(TAG, "Hostname from MAC: %s", h->hostname);
}

/* --- mDNS lifecycle --- */

static void start_mdns(struct f_mdns *h)
{
    if (h->running) return;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(h->hostname);
    mdns_instance_name_set(MDNS_INSTANCE);

    /* _coap._udp — CoAP server */
    mdns_service_add(NULL, "_coap", "_udp", 5683, NULL, 0);

    /* _http._tcp — HTTP/provisioning */
    mdns_txt_item_t http_txt[] = {{"path", "/"}};
    mdns_service_add(NULL, "_http", "_tcp", 80, http_txt, 1);

    /* _espfm._tcp — custom device info */
    char ver_str[8], fw_str[12];
    snprintf(ver_str, sizeof(ver_str), "%d", ESPFM_VERSION_MAJOR);
#ifdef PROJECT_VER
    snprintf(fw_str, sizeof(fw_str), "%.11s", PROJECT_VER);
#else
    snprintf(fw_str, sizeof(fw_str), "dev");
#endif
    mdns_txt_item_t espfm_txt[] = {
        {"version", ver_str},
        {"fw", fw_str},
    };
    mdns_service_add(NULL, "_espfm", "_tcp", 5683, espfm_txt, 2);

    h->running = true;
    ESP_LOGI(TAG, "mDNS started: %s.local [coap:5683 http:80 espfm:5683]", h->hostname);
}

static void stop_mdns(struct f_mdns *h)
{
    if (!h->running) return;
    mdns_free();
    h->running = false;
    ESP_LOGI(TAG, "mDNS stopped");
}

/* --- Event handler --- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct f_mdns *h = (struct f_mdns *)arg;
    if (!h) return;

    if (base == ESPFM_EVENT && id == ESPFM_EVENT_WIFI_CONNECTED) {
        start_mdns(h);
    } else if (base == ESPFM_EVENT && id == ESPFM_EVENT_WIFI_DISCONNECTED) {
        stop_mdns(h);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        start_mdns(h);
    }
}

/* --- Public API --- */

esp_err_t f_mdns_init(f_mdns_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    struct f_mdns *h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;

    load_hostname(h);
    esp_netif_set_hostname(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), h->hostname);

    esp_err_t err;
    err = esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_CONNECTED, on_wifi_event, h);
    if (err != ESP_OK) goto cleanup;
    err = esp_event_handler_register(ESPFM_EVENT, ESPFM_EVENT_WIFI_DISCONNECTED, on_wifi_event, h);
    if (err != ESP_OK) goto cleanup;
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_event, h);
    if (err != ESP_OK) goto cleanup;

    h->running = false;
    s_instance = h;
    *handle    = h;
    ESP_LOGI(TAG, "mDNS initialized (hostname: %s.local)", h->hostname);
    return ESP_OK;

cleanup:
    free(h);
    return err;
}

esp_err_t f_mdns_deinit(f_mdns_handle_t handle)
{
    if (!handle) return ESP_OK;
    if (handle->running) mdns_free();
    esp_event_handler_unregister(ESPFM_EVENT, ESP_EVENT_ANY_ID, on_wifi_event);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, on_wifi_event);
    if (s_instance == handle) s_instance = NULL;
    free(handle);
    return ESP_OK;
}

const char *f_mdns_get_hostname(f_mdns_handle_t handle)
{
    if (!handle) return "espfm";
    return handle->hostname;
}

esp_err_t f_mdns_set_hostname(const char *hostname)
{
    if (!validate_hostname(hostname)) {
        ESP_LOGE(TAG, "Invalid hostname: '%s' (RFC 1035: 1-63 lowercase alphanumeric + hyphens)",
                 hostname ? hostname : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, NVS_KEY_HOSTNAME, hostname);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;

    /* Propagate to in-memory instance and running mDNS */
    if (s_instance) {
        strncpy(s_instance->hostname, hostname, sizeof(s_instance->hostname) - 1);
        s_instance->hostname[sizeof(s_instance->hostname) - 1] = '\0';
        if (s_instance->running) {
            mdns_hostname_set(s_instance->hostname);
        }
    }

    ESP_LOGI(TAG, "Hostname saved to NVS: %s", hostname);
    return ESP_OK;
}
