#include "f_provision_dns.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "f_provision_dns";

#define DNS_PORT     53
#define DNS_BUF_SIZE 512
#define DNS_FLAG_QR  0x8000 /* Response flag */
#define DNS_FLAG_AA  0x0400 /* Authoritative answer */
#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1
#define DNS_TTL_SEC  60

/* Captive portal IP address: 192.168.4.1 */
static const uint8_t CAPTIVE_IP[4] = {192, 168, 4, 1};

static struct udp_pcb *s_dns_pcb   = NULL;

/**
 * @brief Build a DNS response that answers any A-record query with 192.168.4.1.
 *
 * Response layout:
 *   - Header (12 bytes): copy of query header with QR=1, ANCOUNT=1
 *   - Question section: copied verbatim from request
 *   - Answer section: name pointer (0xC00C), type A, class IN, TTL, RDLENGTH=4, RDATA
 */
static void _dns_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                         u16_t port)
{
    if (p == NULL) return;

    /* Minimum DNS query: 12-byte header + at least 1 byte for name + 4 bytes QTYPE+QCLASS */
    if (p->len < 17 || p->len > DNS_BUF_SIZE) {
        ESP_LOGD(TAG, "Ignoring malformed DNS query (len=%u)", p->len);
        pbuf_free(p);
        return;
    }

    const uint8_t *query = (const uint8_t *)p->payload;
    uint16_t qdcount     = ((uint16_t)query[4] << 8) | query[5];

    /* Only handle single-question queries */
    if (qdcount != 1) {
        ESP_LOGD(TAG, "Ignoring DNS query with %u questions", qdcount);
        pbuf_free(p);
        return;
    }

    /* Find end of question name (null-terminated labels) */
    size_t name_end = 12;
    while (name_end < (size_t)p->len && query[name_end] != 0) {
        uint8_t label_len = query[name_end];
        if ((label_len & 0xC0) == 0xC0) {
            /* Compressed pointer — skip 2 bytes */
            name_end += 2;
            goto name_done;
        }
        name_end += (size_t)(1 + label_len);
    }
    if (name_end < (size_t)p->len) {
        name_end++; /* skip the trailing null byte */
    }
name_done:

    /* QTYPE (2 bytes) + QCLASS (2 bytes) */
    size_t question_section_len = name_end - 12 + 4;

    /* Total response length: header(12) + question + answer(16) */
    size_t resp_len = 12 + question_section_len + 16;
    if (resp_len > DNS_BUF_SIZE) {
        ESP_LOGD(TAG, "Response too large (%u bytes)", (unsigned)resp_len);
        pbuf_free(p);
        return;
    }

    uint8_t resp[DNS_BUF_SIZE];
    memset(resp, 0, resp_len);

    /* Copy header, set QR=1 (response), ANCOUNT=1 */
    memcpy(resp, query, 12);
    resp[2] = (uint8_t)((query[2] | 0x80)); /* Set QR bit */
    resp[3] = (uint8_t)((query[3] | 0x04)); /* Set AA bit (authoritative) */
    resp[6] = 0x00;                         /* ANCOUNT high byte */
    resp[7] = 0x01;                         /* ANCOUNT = 1 */
    /* NSCOUNT = 0, ARCOUNT = 0 (already zeroed) */

    /* Copy question section */
    memcpy(resp + 12, query + 12, question_section_len);

    /* Answer section starts after question */
    size_t ans_off = 12 + question_section_len;

    /* Name: pointer to offset 12 (0xC00C) */
    resp[ans_off]     = 0xC0;
    resp[ans_off + 1] = 0x0C;

    /* Type A (1) */
    resp[ans_off + 2] = 0x00;
    resp[ans_off + 3] = 0x01;

    /* Class IN (1) */
    resp[ans_off + 4] = 0x00;
    resp[ans_off + 5] = 0x01;

    /* TTL = 60 seconds (4 bytes, big-endian) */
    resp[ans_off + 6] = 0x00;
    resp[ans_off + 7] = 0x00;
    resp[ans_off + 8] = 0x00;
    resp[ans_off + 9] = (uint8_t)DNS_TTL_SEC;

    /* RDLENGTH = 4 */
    resp[ans_off + 10] = 0x00;
    resp[ans_off + 11] = 0x04;

    /* RDATA = 192.168.4.1 */
    resp[ans_off + 12] = CAPTIVE_IP[0];
    resp[ans_off + 13] = CAPTIVE_IP[1];
    resp[ans_off + 14] = CAPTIVE_IP[2];
    resp[ans_off + 15] = CAPTIVE_IP[3];

    /* Send response */
    struct pbuf *resp_pbuf = pbuf_alloc(PBUF_TRANSPORT, (u16_t)resp_len, PBUF_RAM);
    if (resp_pbuf != NULL) {
        memcpy(resp_pbuf->payload, resp, resp_len);
        udp_sendto(pcb, resp_pbuf, addr, port);
        pbuf_free(resp_pbuf);
        ESP_LOGD(TAG, "DNS redirect -> %u.%u.%u.%u (query from %u.%u.%u.%u:%u)", CAPTIVE_IP[0],
                 CAPTIVE_IP[1], CAPTIVE_IP[2], CAPTIVE_IP[3], ip4_addr1(ip_2_ip4(addr)),
                 ip4_addr2(ip_2_ip4(addr)), ip4_addr3(ip_2_ip4(addr)), ip4_addr4(ip_2_ip4(addr)),
                 (unsigned)port);
    } else {
        ESP_LOGW(TAG, "Failed to allocate DNS response pbuf");
    }

    pbuf_free(p);
}

esp_err_t f_provision_dns_start(void)
{
    if (s_dns_pcb != NULL) {
        ESP_LOGW(TAG, "DNS server already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_dns_pcb = udp_new();
    if (s_dns_pcb == NULL) {
        ESP_LOGE(TAG, "Failed to create UDP PCB");
        return ESP_ERR_NO_MEM;
    }

    err_t err = udp_bind(s_dns_pcb, IP_ADDR_ANY, DNS_PORT);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "Failed to bind UDP port %d: %d", DNS_PORT, (int)err);
        udp_remove(s_dns_pcb);
        s_dns_pcb = NULL;
        return ESP_FAIL;
    }

    udp_recv(s_dns_pcb, _dns_recv_cb, NULL);

    ESP_LOGI(TAG, "DNS redirect server started on port %d -> %u.%u.%u.%u", DNS_PORT, CAPTIVE_IP[0],
             CAPTIVE_IP[1], CAPTIVE_IP[2], CAPTIVE_IP[3]);
    return ESP_OK;
}

void f_provision_dns_stop(void)
{
    if (s_dns_pcb != NULL) {
        udp_remove(s_dns_pcb);
        s_dns_pcb = NULL;
        ESP_LOGI(TAG, "DNS redirect server stopped");
    }
}
